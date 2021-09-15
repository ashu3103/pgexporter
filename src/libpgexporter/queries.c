/*
 * Copyright (C) 2021 Red Hat
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgexporter */
#include <pgexporter.h>
#include <logging.h>
#include <message.h>
#include <network.h>
#include <queries.h>
#include <security.h>
#include <utils.h>

/* system */
#include <stdlib.h>

static int query_execute(int server, char* query, char* tag, int columns, struct tuples** tuples);
static void* data_append(void* orig, size_t orig_size, void* n, size_t n_size);
static int create_D_tuple(int server, int columns, char* tag, struct message* msg, struct tuples** tuples);

void
pgexporter_open_connections(void)
{
   int ret;
   int user;
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (int server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd == -1)
      {
         user = -1;
         for (int usr = 0; user == -1 && usr < config->number_of_users; usr++)
         {
            if (!strcmp(&config->users[usr].username[0], &config->servers[server].username[0]))
            {
               user = usr;
            }
         }

         ret = pgexporter_server_authenticate(server, "postgres",
                                              &config->users[user].username[0], &config->users[user].password[0],
                                              &config->servers[server].fd);
         if (ret != AUTH_SUCCESS)
         {
            pgexporter_log_error("Failed login for '%s' on server '%s'", &config->users[user].username, &config->servers[server].name);
         }
      }
   }
}

void
pgexporter_close_connections(void)
{
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (int server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd != -1)
      {
         pgexporter_write_terminate(NULL, config->servers[server].fd);
         pgexporter_disconnect(config->servers[server].fd);
         config->servers[server].fd = -1;
      }
   }
}

int
pgexporter_query_database_size(int server, struct tuples** tuples)
{
   return query_execute(server, "SELECT datname, pg_database_size(datname) FROM pg_database;",
                        "pg_database", 2, tuples);
}

int
pgexporter_query_replication_slot_active(int server, struct tuples** tuples)
{
   return query_execute(server, "SELECT slot_name,active FROM pg_replication_slots;",
                        "pg_replication_slots", 2, tuples);
}

int
pgexporter_query_settings(int server, struct tuples** tuples)
{
   return query_execute(server, "SELECT name,setting,short_desc FROM pg_settings;",
                        "pg_settings", 3, tuples);
}

struct tuples*
pgexporter_merge_tuples(struct tuples* t1, struct tuples* t2)
{
   struct tuples* last = NULL;
   struct tuples* ct1 = NULL;
   struct tuples* ct2 = NULL;
   struct tuples* tmp1 = NULL;
   struct tuples* tmp2 = NULL;

   if (t1 == NULL)
   {
      return t2;
   }

   if (t2 == NULL)
   {
      return t1;
   }

   ct1 = t1;
   ct2 = t2;

   while (ct1 != NULL)
   {
      if (ct2 != NULL && !strcmp(&ct1->tuple->name[0], &ct2->tuple->name[0]))
      {
         while (ct1->next != NULL && !strcmp(&ct1->next->tuple->name[0], &ct2->tuple->name[0]))
         {
            ct1 = ct1->next;
         }

         tmp1 = ct1->next;
         tmp2 = ct2->next;

         ct1->next = ct2;
         ct2->next = tmp1;
         ct2 = tmp2;
      }

      last = ct1;
      ct1 = ct1->next;
   }

   while (ct2 != NULL)
   {
      last->next = ct2;

      last = last->next;
      ct2 = ct2->next;
   }

   return t1;
}

int
pgexporter_free_tuples(struct tuples* tuples)
{
   struct tuples* current = NULL;
   struct tuples* next = NULL;

   current = tuples;

   while (current != NULL)
   {
      next = current->next;

      free(current->tuple);
      free(current);

      current = next;
   }

   return 0;
}

static int
query_execute(int server, char* query, char* tag, int columns, struct tuples** tuples)
{
   int status;
   bool cont;
   struct message qmsg = {0};
   size_t size = 0;
   char* content = NULL;
   struct message* msg = NULL;
   struct tuples* root = NULL;
   struct tuples* current = NULL;
   void* data = NULL;
   size_t data_size = 0;
   size_t offset = 0;
   struct configuration* config;

   config = (struct configuration*)shmem;

   memset(&qmsg, 0, sizeof(struct message));

   size = 1 + 4 + strlen(query) + 1;
   content = (char*)malloc(size);
   memset(content, 0, size);

   pgexporter_write_byte(content, 'Q');
   pgexporter_write_int32(content + 1, size - 1);
   pgexporter_write_string(content + 5, query);

   qmsg.kind = 'Q';
   qmsg.length = size;
   qmsg.data = content;

   status = pgexporter_write_message(NULL, config->servers[server].fd, &qmsg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto error;
   }

   cont = true;
   while (cont)
   {
      status = pgexporter_read_block_message(NULL, config->servers[server].fd, &msg);

      if (status == MESSAGE_STATUS_OK)
      {
         data = data_append(data, data_size, msg->data, msg->length);
         data_size += msg->length;

         if (pgexporter_has_message('Z', data, data_size))
         {
            cont = false;
         }
      }
      else
      {
         goto error;
      }

      pgexporter_free_message(msg);
      msg = NULL;
   }

   while (offset < data_size)
   {
      offset = pgexporter_extract_message_offset(offset, data, &msg);

      if (msg != NULL && msg->kind == 'D')
      {
         struct tuples* dtuples = NULL;

         create_D_tuple(server, columns, tag, msg, &dtuples);

         if (root == NULL)
         {
            root = dtuples;
            current = root;
         }
         else
         {
            current->next = dtuples;
            current = current->next;
         }
      }

      pgexporter_free_message(msg);
      msg = NULL;
   }

   *tuples = root;

   free(data);

   return 0;

error:

   pgexporter_free_message(msg);
   free(data);

   return 1;
}

static void*
data_append(void* orig, size_t orig_size, void* n, size_t n_size)
{
   void* d = NULL;

   if (n != NULL)
   {
      d = realloc(orig, orig_size + n_size);
      memcpy(d + orig_size, n, n_size); 
   }

   return d;
}

static int
create_D_tuple(int server, int columns, char* tag, struct message* msg, struct tuples** tuples)
{
   int offset;
   int length;
   struct tuples* result = NULL;
   struct tuple* d = NULL;

   result = (struct tuples*)malloc(sizeof(struct tuples));
   d = (struct tuple*)malloc(sizeof(struct tuple));

   memset(result, 0, sizeof(struct tuples));
   memset(d, 0, sizeof(struct tuple));

   d->server = server;
   d->columns = columns;
   memcpy(&d->tag[0], tag, strlen(tag));
   
   offset = 7;

   length = pgexporter_read_int32(msg->data + offset);
   offset += 4;

   if (length > 0)
   {
      memcpy(&d->name[0], msg->data + offset, length);
      offset += length;
   }

   if (columns >= 2)
   {
      length = pgexporter_read_int32(msg->data + offset);
      offset += 4;

      if (length > 0)
      {
         memcpy(&d->value[0], msg->data + offset, length);
         offset += length;
      }
   }

   if (columns >= 3)
   {
      length = pgexporter_read_int32(msg->data + offset);
      offset += 4;

      if (length > 0)
      {
         memcpy(&d->desc[0], msg->data + offset, length);
         offset += length;
      }
   }

   result->tuple = d;
   result->next = NULL;

   *tuples = result;

   return 0;
}
