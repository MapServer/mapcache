/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache connection pooling
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <apr_reslist.h>
#include "mapcache.h"

struct mapcache_connection_pool {
    apr_pool_t *server_pool;
    apr_reslist_t *connexions;
};


struct mapcache_pooled_connection_container {
  mapcache_pooled_connection *head;
  apr_pool_t *pool;
  unsigned int max_list_size;
};



struct mapcache_pooled_connection_private_data {
  char *key;
  mapcache_connection_destructor destructor; 
  mapcache_pooled_connection *next;
  mapcache_pooled_connection_container *pcc;
};

static apr_status_t mapcache_connection_container_creator(void **conn_, void *params, apr_pool_t *pool) {
  mapcache_pooled_connection_container *pcc;
  pcc = calloc(1, sizeof(mapcache_pooled_connection_container));
  pcc->max_list_size = 10;
  pcc->pool = pool;
  *conn_ = pcc;
  return APR_SUCCESS;
}

static apr_status_t mapcache_connection_container_destructor(void *conn_, void *params, apr_pool_t *pool) {
  mapcache_pooled_connection_container *pcc = (mapcache_pooled_connection_container*)conn_;
  mapcache_pooled_connection *pc = pcc->head;
  while(pc) {
    mapcache_pooled_connection *this = pc;
    this->private->destructor(this->connection);
    free(this->private->key);
    pc = this->private->next;
    free(this);
  }
  free(pcc);
  return MAPCACHE_SUCCESS;
}


apr_status_t mapcache_connection_pool_create(mapcache_connection_pool **cp, apr_pool_t *server_pool) {
  apr_status_t rv;
  *cp = apr_pcalloc(server_pool, sizeof(mapcache_connection_pool));
  (*cp)->server_pool = server_pool;
  rv = apr_reslist_create(&((*cp)->connexions), 1, 5, 1024, 60*1000000,
      mapcache_connection_container_creator,
      mapcache_connection_container_destructor,
      NULL,
      server_pool);
  return rv;
}

mapcache_pooled_connection* mapcache_connection_pool_get_connection(mapcache_context *ctx, char *key,
        mapcache_connection_constructor constructor, mapcache_connection_destructor destructor,
        void *params) {
  apr_status_t rv;
  int count = 0;
  mapcache_pooled_connection_container *pcc;
  mapcache_pooled_connection *pc,*pred=NULL;
  rv = apr_reslist_acquire(ctx->connection_pool->connexions, (void**)&pcc);
  if(rv != APR_SUCCESS || !pcc) {
    char errmsg[120];
    ctx->set_error(ctx,500, "failed to acquire connection from mapcache connection pool: (%s)", apr_strerror(rv, errmsg,120));
    return NULL;
  }

  /* loop through existing connections to see if we find one matching the given key */
  pc = pcc->head;
  while(pc) {
    count++;
    if(!strcmp(key,pc->private->key)) {
      /* move current connection to head of list, and return it. We only move the connection
         to the front of the list if it wasn't in the first 2 connections, as in the seeding
         case we are always alternating between read and write operations (i.e. potentially
         2 different connections and in that cas we end up switching connections each time
         there's an access */
      if(pc != pcc->head && count>2) {
        assert(pred);
        pred->private->next = pc->private->next;
        pc->private->next = pcc->head;
        pcc->head = pc;
      }
      return pc;
    }
    pred = pc;
    pc = pc->private->next;
  }
  
  /* connection not found in pool */
  pc = calloc(1,sizeof(mapcache_pooled_connection));
  /*
  ctx->log(ctx, MAPCACHE_DEBUG, "calling constructor for pooled connection (%s)", key);
  */
  constructor(ctx, &pc->connection, params);
  if(GC_HAS_ERROR(ctx)) {
    free(pc);
    apr_reslist_release(ctx->connection_pool->connexions, pcc);
    return NULL;
  }
  
  pc->private = calloc(1,sizeof(mapcache_pooled_connection_private_data));
  pc->private->key = strdup(key);
  pc->private->destructor = destructor;
  pc->private->next = pcc->head;
  pc->private->pcc = pcc;
  
  if(count == pcc->max_list_size) {
    /* max number of connections atained, we must destroy the last one that was used */
    mapcache_pooled_connection *opc;
    opc = pcc->head;
    count = 1;
    while(count < pcc->max_list_size) {
      pred = opc;
      opc = opc->private->next;
      count++;
    }
    ctx->log(ctx, MAPCACHE_DEBUG, "tearing down pooled connection (%s) to make room", opc->private->key);
    opc->private->destructor(opc->connection);
    free(opc->private->key);
    free(opc->private);
    free(opc);
    if(pred) {
      pred->private->next = NULL;
    }
  }
  pcc->head = pc;
  return pc;

}
void mapcache_connection_pool_invalidate_connection(mapcache_context *ctx, mapcache_pooled_connection *connection) {
  mapcache_pooled_connection_container *pcc = connection->private->pcc;
  mapcache_pooled_connection *pc = pcc->head, *pred=NULL;
  while(pc) {
    if(pc == connection) {
      if(pred) {
        pred->private->next = pc->private->next;
      } else {
        pcc->head = pc->private->next;
      }
      pc->private->destructor(pc->connection);
      free(pc->private->key);
      free(pc);
      break;
    }
    pred = pc;
    pc = pc->private->next;
  }
  apr_reslist_release(ctx->connection_pool->connexions,(void*)pcc);
}

void mapcache_connection_pool_release_connection(mapcache_context *ctx, mapcache_pooled_connection *connection) {
  if(connection) {
    mapcache_pooled_connection_container *pcc = connection->private->pcc;
    apr_reslist_release(ctx->connection_pool->connexions,(void*)pcc);
  }
}

