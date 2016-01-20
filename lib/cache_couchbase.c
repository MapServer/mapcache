/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: couchbase cache backend.
 * Author:   Michael Downey and the MapServer team.
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

#include "mapcache.h"

#ifdef USE_COUCHBASE

#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <libcouchbase/couchbase.h>

typedef struct mapcache_cache_couchbase mapcache_cache_couchbase;

/**\class mapcache_cache_couchbase
 * \brief a mapcache_cache on couchbase servers
 * \implements mapcache_cache
 */
struct mapcache_cache_couchbase {
   mapcache_cache cache;
//   apr_reslist_t *connection_pool;
   char *host;
   char *username;
   char *password;
   char *bucket;
   mapcache_context *ctx;
};


typedef struct getStruct
{
   mapcache_buffer *tileBuffer;
   libcouchbase_error_t error;
} getStruct_t;

/* Not sure if we need this. */
static void _couchbase_error_callback(libcouchbase_t instance,
                                      libcouchbase_error_t error,
                                      const char *errinfo)
{
   /* Ignore timeouts... */
   if (error != LIBCOUCHBASE_ETIMEDOUT) {
      fprintf(stderr, "\nFATAL ERROR: %s\n",
              libcouchbase_strerror(instance, error));
      if (errinfo && strlen(errinfo) != 0) {
         fprintf(stderr, "\t\"%s\"\n", errinfo);
      }
   }
}

static void _couchbase_get_callback(libcouchbase_t instance,
                                    const void *cookie,
                                    libcouchbase_error_t error,
                                    const void *key, libcouchbase_size_t nkey,
                                    const void *bytes, libcouchbase_size_t nbytes,
                                    libcouchbase_uint32_t flags, libcouchbase_cas_t cas)
{
   (void)instance;
   (void)key;
   (void)nkey;
   (void)flags;
   (void)cas;

   if (cookie)
   {
       getStruct_t *request = (getStruct_t*)cookie;

       request->error = error;

       if (error == LIBCOUCHBASE_SUCCESS && request->tileBuffer)
       {
           mapcache_buffer_append(request->tileBuffer, nbytes, (void*)bytes);
       }
    }
}

static void _couchbase_store_callback(libcouchbase_t instance,
                                      const void* cookie,
                                      libcouchbase_storage_t unknown,
                                      libcouchbase_error_t error,
                                      const void* unknown2,
                                      libcouchbase_size_t unknown3,
                                      libcouchbase_cas_t cas)
{
   (void)instance;
   (void)unknown;
   (void)unknown2;
   (void)unknown3;
   (void)cas;

   libcouchbase_error_t* userError = (libcouchbase_error_t*)cookie;

   *userError = error;
}

static apr_status_t _couchbase_reslist_get_connection(void **conn_, void *params, apr_pool_t *pool) {
   mapcache_cache_couchbase *cache = (mapcache_cache_couchbase*)params;

   libcouchbase_t *instance = apr_pcalloc(pool,sizeof(libcouchbase_t));
   const char *host = cache->host;
   const char *username = cache->username;
   const char *passwd = cache->password;
   const char *bucket = "default";

  *instance = libcouchbase_create(host, username, passwd, bucket, NULL);
   if (*instance == NULL) {
      return APR_EGENERAL;
   } 

   libcouchbase_set_error_callback(*instance, _couchbase_error_callback);
   libcouchbase_set_get_callback(*instance, _couchbase_get_callback);
   libcouchbase_set_storage_callback(*instance, _couchbase_store_callback);

   if (libcouchbase_connect(*instance) != LIBCOUCHBASE_SUCCESS) {
       return APR_EGENERAL;
   }
   
   /* Wait for the connect to compelete */
   libcouchbase_wait(*instance);

   *conn_ = instance;
   return APR_SUCCESS;
}

static apr_status_t _couchbase_reslist_free_connection(void *conn_, void *params, apr_pool_t *pool) {
   libcouchbase_t *instance = (libcouchbase_t*)conn_;
   libcouchbase_destroy(*instance);
   return APR_SUCCESS; 
}

static libcouchbase_t* _couchbase_get_connection(mapcache_context *ctx, mapcache_tile *tile)
{
   apr_status_t rv;
   libcouchbase_t *instance;
   mapcache_cache_couchbase *cache = (mapcache_cache_couchbase*)tile->tileset->cache;
   
   rv = apr_reslist_acquire(cache->connection_pool, (void **)&instance);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx, 500, "failed to aquire connection to couchbase backend: %s", ctx->get_error_message(ctx));
      return NULL;
   }

   return instance;
}

static void _couchbase_release_connection(mapcache_tile *tile, libcouchbase_t* instance)
{
   mapcache_cache_couchbase* cache = (mapcache_cache_couchbase*)tile->tileset->cache;
   apr_reslist_release(cache->connection_pool, (void*)instance);
}

static void _couchbase_invalidate_connection(mapcache_tile *tile, libcouchbase_t* instance)
{
   mapcache_cache_couchbase* cache = (mapcache_cache_couchbase*)tile->tileset->cache;
   apr_reslist_invalidate(cache->connection_pool, (void*)instance);
}

static int _mapcache_cache_couchbase_has_tile(mapcache_context *ctx, mapcache_tile *tile) {
   char *key[1];
   libcouchbase_t *instance;
   libcouchbase_error_t error;
   size_t keySize[1];
   getStruct_t request;

   key[0] = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
   if(GC_HAS_ERROR(ctx)) {
      return MAPCACHE_FALSE;
   }

   keySize[0] = strlen(key[0]);

   instance = _couchbase_get_connection(ctx, tile);
   request.tileBuffer = 0;
   request.error = LIBCOUCHBASE_KEY_ENOENT;
   error = libcouchbase_mget(*instance, &request, 1, (const void * const*)key, keySize, 0);
   if (error != LIBCOUCHBASE_SUCCESS) {
      ctx->set_error(ctx, 500, "couchbase: failed to get key %s: %s", key, libcouchbase_strerror(*instance, error));
      _couchbase_invalidate_connection(tile, instance);
      return MAPCACHE_FALSE;
   }
   
   libcouchbase_wait(*instance);

   error = request.error;
   if (error != LIBCOUCHBASE_SUCCESS) {
      ctx->set_error(ctx, 500, "couchbase: failed to get key %s: %s", key, libcouchbase_strerror(*instance, error));
      _couchbase_invalidate_connection(tile, instance);
      return MAPCACHE_FALSE;
   }

   _couchbase_release_connection(tile, instance);
   return MAPCACHE_TRUE;
}

static void _mapcache_cache_couchbase_delete(mapcache_context *ctx, mapcache_tile *tile) {
   char *key;
   libcouchbase_t *instance;
   libcouchbase_error_t error;

   key = mapcache_util_get_tile_key(ctx, tile,NULL," \r\n\t\f\e\a\b","#");
   GC_CHECK_ERROR(ctx);

   instance = _couchbase_get_connection(ctx, tile);

   error = libcouchbase_remove(*instance, 0, key, strlen(key), 0);
   if (error != LIBCOUCHBASE_SUCCESS) {
      ctx->set_error(ctx, 500, "couchbase: failed to delete key %s: %s", key, libcouchbase_strerror(*instance, error));
   }
   
   libcouchbase_wait(*instance);

   error = libcouchbase_get_last_error(*instance);
   if (error != LIBCOUCHBASE_SUCCESS) {
      ctx->set_error(ctx, 500, "couchbase: failed to delete key %s: %s", key, libcouchbase_strerror(*instance, error));
   }

   _couchbase_release_connection(tile, instance);
}

/**
 * \brief get content of given tile
 * 
 * fills the mapcache_tile::data of the given tile with content stored on the couchbase server
 * \private \memberof mapcache_cache_couchbase
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_couchbase_get(mapcache_context *ctx, mapcache_tile *tile) {
   char *key[1];
   size_t keySize[1];
   libcouchbase_t *instance;
   libcouchbase_error_t error;
   getStruct_t request;

   key[0] = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
   if(GC_HAS_ERROR(ctx)) {
      return MAPCACHE_FAILURE;
   }
   
   keySize[0] = strlen(key[0]);

   tile->encoded_data = mapcache_buffer_create(0, ctx->pool);

   libcouchbase_time_t expires = 86400;
   if(tile->tileset->auto_expire)
      expires = tile->tileset->auto_expire;

   instance = _couchbase_get_connection(ctx, tile);
   if (GC_HAS_ERROR(ctx)) {
      return MAPCACHE_FAILURE;
   }

   request.tileBuffer = tile->encoded_data;
   error = libcouchbase_mget(*instance, &request, 1, (const void * const*)key, keySize, &expires);
   if (error != LIBCOUCHBASE_SUCCESS) {
      ctx->set_error(ctx, 500, "couchbase cache returned error on mget %s", libcouchbase_strerror(*instance, error));
      _couchbase_invalidate_connection(tile, instance);
      return MAPCACHE_FAILURE;
   }
   
   libcouchbase_wait(*instance);

   if(request.error != LIBCOUCHBASE_SUCCESS) {
       _couchbase_release_connection(tile, instance);
       return MAPCACHE_CACHE_MISS;
   }

   if (tile->encoded_data->size == 0) {
      _couchbase_release_connection(tile, instance);
      ctx->set_error(ctx, 500, "couchbase cache returned 0-length data for tile %d %d %d", tile->x, tile->y, tile->z);
      return MAPCACHE_FAILURE;
   }
   
   apr_time_t now = apr_time_now();
   tile->mtime = now;

   _couchbase_release_connection(tile, instance);
   return MAPCACHE_SUCCESS;
}

/**
 * \brief push tile data to couchbase
 * 
 * writes the content of mapcache_tile::data to the configured couchbased instance(s)
 * \private \memberof mapcache_cache_couchbase
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_couchbase_set(mapcache_context *ctx, mapcache_tile *tile) {
   char *key;
   libcouchbase_t *instance;
   libcouchbase_error_t error;
   const int max_retries = 3;
   int retries = max_retries;
   apr_interval_time_t delay;

   /* set expiration to one day if not configured */
   libcouchbase_time_t expires = 86400;
   if(tile->tileset->auto_expire)
      expires = tile->tileset->auto_expire;

   mapcache_cache_couchbase *cache = (mapcache_cache_couchbase*)tile->tileset->cache;
   key = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
   GC_CHECK_ERROR(ctx);
   
   if(!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
   }
   
   instance = _couchbase_get_connection(ctx, tile);
   GC_CHECK_ERROR(ctx);

   do
   {
      error = libcouchbase_store(*instance, &error, LIBCOUCHBASE_SET, key, strlen(key), tile->encoded_data->buf, tile->encoded_data->size, 0, expires, 0);
      if (error != LIBCOUCHBASE_SUCCESS) {
          _couchbase_release_connection(tile, instance);
          ctx->set_error(ctx, 500, "failed to store tile %d %d %d to couchbase cache %s due to eror %s.",
                         tile->x, tile->y, tile->z, cache->cache.name, libcouchbase_strerror(*instance, error));
          return;
      }
   
      libcouchbase_wait(*instance);

      if (error == LIBCOUCHBASE_ETMPFAIL) {
          if (retries > 0) {
              delay = 100000 * (1 << (max_retries - retries));	// Do an exponential back off of starting at 100 milliseconds
              apr_sleep(delay);
          }
          else {
              _couchbase_release_connection(tile, instance);
              ctx->set_error(ctx, 500, "failed to store tile %d %d %d to couchbase cache %s due to %s. Maximum number of retries used.",
                             tile->x, tile->y, tile->z, cache->cache.name, libcouchbase_strerror(*instance, error));
              return;
          }

          --retries;
      }

      else if (error != LIBCOUCHBASE_SUCCESS) {
          _couchbase_release_connection(tile, instance);
          ctx->set_error(ctx, 500, "failed to store tile %d %d %d to couchbase cache %s due to error %s.",
                         tile->x, tile->y, tile->z, cache->cache.name, libcouchbase_strerror(*instance, error));
          return;
      }
   }
   while (error == LIBCOUCHBASE_ETMPFAIL);

   _couchbase_release_connection(tile, instance);
}

/**
 * \private \memberof mapcache_cache_couchbase
 */
static void _mapcache_cache_couchbase_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
   ezxml_t cur_node;
   apr_status_t rv;
   mapcache_cache_couchbase *dcache = (mapcache_cache_couchbase*)cache;
   int servercount = 0;

   for(cur_node = ezxml_child(node,"server"); cur_node; cur_node = cur_node->next) {
      servercount++;
   }

   if(!servercount) {
      ctx->set_error(ctx, 400, "couchbase cache %s has no <server>s configured", cache->name);
      return;
   }

   if(servercount > 1) {
      ctx->set_error(ctx, 400, "couchbase cache %s has more than 1 server configured", cache->name);
      return;
   }

   cur_node = ezxml_child(node, "server");
   ezxml_t xhost = ezxml_child(cur_node, "host");   /* Host should contain server:port */
   ezxml_t xusername = ezxml_child(cur_node, "username");
   ezxml_t xpasswd = ezxml_child(cur_node, "password");
   ezxml_t xbucket = ezxml_child(cur_node, "bucket");

   if(!xhost || !xhost->txt || ! *xhost->txt) {
      ctx->set_error(ctx, 400, "cache %s: <server> with no <host>", cache->name);
      return;
   } else {
      dcache->host = apr_pstrdup(ctx->pool, xhost->txt);
      if (dcache->host == NULL) {
          ctx->set_error(ctx, 400, "cache %s: failed to allocate host string!", cache->name);
          return;
      }
   }

   if(xusername && xusername->txt && *xusername->txt) {
      dcache->username = apr_pstrdup(ctx->pool, xusername->txt);
   }
   
   if(xpasswd && xpasswd->txt && *xpasswd->txt) {
      dcache->password = apr_pstrdup(ctx->pool, xpasswd->txt);
   }
   
   if(xbucket && xbucket->txt && *xbucket->txt) {
      dcache->bucket = apr_pstrdup(ctx->pool, xbucket->txt);
   }

   dcache->ctx = ctx;

   rv = apr_reslist_create(&(dcache->connection_pool),
         0 /* min */,
         10 /* soft max */,
         200 /* hard max */,
         60*1000000 /*60 seconds, ttl*/,
         _couchbase_reslist_get_connection, /* resource constructor */
         _couchbase_reslist_free_connection, /* resource destructor */
         dcache, ctx->pool);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx, 500, "failed to create couchbase connection pool");
      return;
   }
}

/**
 * \private \memberof mapcache_cache_couchbase
 */
static void _mapcache_cache_couchbase_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache, mapcache_cfg *cfg) {
}

/**
 * \brief creates and initializes a mapcache_couchbase_cache
 */
mapcache_cache* mapcache_cache_couchbase_create(mapcache_context *ctx) {
   mapcache_cache_couchbase *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_couchbase));
   if(!cache) {
      ctx->set_error(ctx, 500, "failed to allocate couchbase cache");
      return NULL;
   }

   cache->cache.metadata = apr_table_make(ctx->pool, 3);
   cache->cache.type = MAPCACHE_CACHE_COUCHBASE;
   cache->cache.tile_get = _mapcache_cache_couchbase_get;
   cache->cache.tile_exists = _mapcache_cache_couchbase_has_tile;
   cache->cache.tile_set = _mapcache_cache_couchbase_set;
   cache->cache.tile_delete = _mapcache_cache_couchbase_delete;
   cache->cache.configuration_parse_xml = _mapcache_cache_couchbase_configuration_parse_xml;
   cache->cache.configuration_post_config = _mapcache_cache_couchbase_configuration_post_config;
   cache->host = NULL;
   cache->username = NULL;
   cache->password = NULL;
   cache->bucket = NULL;

   return (mapcache_cache*)cache;
}

#else
mapcache_cache* mapcache_cache_couchbase_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"COUCHBASE support not compiled in this version");
  return NULL;
}
#endif

/* vim: ai ts=3 sts=3 et sw=3
*/
