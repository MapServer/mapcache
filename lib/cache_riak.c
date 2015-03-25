/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: riak cache backend.
 * Author:   Michael Downey and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2013 Regents of the University of Minnesota.
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

#include "mapcache-config.h"
#ifdef USE_RIAK

#include "mapcache.h"

#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_hash.h>
#ifdef APR_HAS_THREADS
#include <apr_thread_mutex.h>
#endif

#include <string.h>
#include <errno.h>

/*
 * Since we don't construct the connection pool and store it in the cache object
 * we have to store all the connections in a hash map in case there are multiple
 * riak caches defined.
 */
static apr_hash_t *_connection_pools = NULL;

static apr_status_t _riak_reslist_get_connection(void **conn_, void *params, apr_pool_t *pool) {
    mapcache_cache_riak *cache = (mapcache_cache_riak*)params;

    struct RIACK_CLIENT *client = riack_new_client(0);
    if (client == NULL) {
        return APR_EGENERAL;
    }

    struct RIACK_CONNECTION_OPTIONS options;
    options.recv_timeout_ms = 2000;
    options.send_timeout_ms = 2000;
    if (riack_connect(client, cache->host, cache->port, &options) != RIACK_SUCCESS) {
        riack_free(client);
        return APR_EGENERAL;
    }

    if (riack_ping(client) != RIACK_SUCCESS) {
        riack_free(client);
        return APR_EGENERAL;
    }

    *conn_ = client;

    return APR_SUCCESS;
}

static apr_status_t _riak_reslist_free_connection(void *conn_, void *params, apr_pool_t *pool) {
    struct RIACK_CLIENT *client = (struct RIACK_CLIENT *)conn_;
    riack_free(client);

    return APR_SUCCESS;
}

static struct RIACK_CLIENT* _riak_get_connection(mapcache_context *ctx, mapcache_cache_riak *cache, mapcache_tile *tile)
{
  apr_time_t start = apr_time_now();

  apr_status_t rv;
  struct RIACK_CLIENT *client = 0;
  apr_reslist_t *pool = NULL;

  if(!_connection_pools || NULL == (pool = apr_hash_get(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING)) ) {
#ifdef APR_HAS_THREADS
    if(ctx->threadlock)
      apr_thread_mutex_lock((apr_thread_mutex_t*)ctx->threadlock);
#endif

    if(!_connection_pools) {
      _connection_pools = apr_hash_make(ctx->process_pool);
    }

    /* probably doesn't exist, unless the previous mutex locked us, so we check */
    pool = apr_hash_get(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING);
    if(!pool) {
      /* there where no existing connection pools, create them*/
      rv = apr_reslist_create(&pool,
                              0 /* min */,
                              50 /* soft max */,
                              200 /* hard max */,
                              60*1000000 /*60 seconds, ttl*/,
                              _riak_reslist_get_connection, /* resource constructor */
                              _riak_reslist_free_connection, /* resource destructor */
                              cache, ctx->process_pool);
      if(rv != APR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to create riak connection pool");
#ifdef APR_HAS_THREADS
        if(ctx->threadlock)
          apr_thread_mutex_unlock((apr_thread_mutex_t*)ctx->threadlock);
#endif
        return NULL;
      }

      apr_hash_set(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING, pool);
    }

#ifdef APR_HAS_THREADS
    if(ctx->threadlock)
      apr_thread_mutex_unlock((apr_thread_mutex_t*)ctx->threadlock);
#endif

    pool = apr_hash_get(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING);
    assert(pool);
  }

  rv = apr_reslist_acquire(pool, (void **)&client);
  if(rv != APR_SUCCESS) {
    ctx->set_error(ctx, 500, "failed to aquire connection to riak backend: %s", ctx->get_error_message(ctx));
    ctx->log(ctx, MAPCACHE_ERROR, "Failed to aquire connection to riak backend: %s", ctx->get_error_message(ctx));
    return NULL;
  }

  int took = apr_time_as_msec(apr_time_now() - start);

  if ( took >= 50 ) {
      ctx->log(ctx, MAPCACHE_DEBUG, "Riak get connection took %d msecs",  took);
  }

  return client;
}

static void _riak_release_connection(mapcache_tile *tile, mapcache_cache_riak *cache, struct RIACK_CLIENT *client)
{
  apr_reslist_t *pool;
  pool = apr_hash_get(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING);
  apr_reslist_release(pool, (void*)client);
}

static void _riak_invalidate_connection(mapcache_tile *tile, mapcache_cache_riak *cache, struct RIACK_CLIENT *client)
{
  apr_reslist_t *pool;
  pool = apr_hash_get(_connection_pools, cache->cache.name, APR_HASH_KEY_STRING);
  apr_reslist_invalidate(pool, (void*)client);
}

static int _mapcache_cache_riak_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    int error;
	int retries = 3;
    RIACK_STRING key;
    struct RIACK_GET_OBJECT obj;
    struct RIACK_CLIENT *client;

    mapcache_cache_riak *cache = (mapcache_cache_riak*)pcache;

    key.value = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FALSE;
    }
    key.len = strlen(key.value);

    client = _riak_get_connection(ctx, cache, tile);
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FALSE;
    }

	do
    {
        error = riack_get(client, cache->bucket, key, 0, &obj);
        if (error != RIACK_SUCCESS) {
            ctx->log(ctx, MAPCACHE_WARN, "Retry %d in riak_has_tile for tile %s from cache %s due to error %d", (4-retries), key.value, cache->cache.name, error);
            for (error = riack_reconnect(client);
                 error != RIACK_SUCCESS && retries > 0;
                 error = riack_reconnect(client))
            {
              --retries;
            }

            --retries;
        }
    }
    while (error != RIACK_SUCCESS && retries >= 0);

    if (error != RIACK_SUCCESS) {
        riack_free_get_object(client, &obj);    // riack_get allocates the returned object so we need to deallocate it.
        _riak_invalidate_connection(tile, cache, client);
        ctx->set_error(ctx, 500, "riak: failed to get key %s: %d", key, error);
        return MAPCACHE_FALSE;
    }

    if (obj.object.content_count < 1 || obj.object.content[0].data_len == 0) {
        riack_free_get_object(client, &obj);    // riack_get allocates the returned object so we need to deallocate it.
        _riak_release_connection(tile, cache, client);
        return MAPCACHE_FALSE;
    }

    riack_free_get_object(client, &obj);    // riack_get allocates the returned object so we need to deallocate it.
    _riak_release_connection(tile, cache, client);

    return MAPCACHE_TRUE;
}

static void _mapcache_cache_riak_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    int error;
    RIACK_STRING key;
    struct RIACK_CLIENT *client;
    struct RIACK_DEL_PROPERTIES properties;

    memset(&properties, 0, sizeof(struct RIACK_DEL_PROPERTIES));

    mapcache_cache_riak *cache = (mapcache_cache_riak*)pcache;

    key.value = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
    GC_CHECK_ERROR(ctx);
    key.len = strlen(key.value);

    client = _riak_get_connection(ctx, cache, tile);
    GC_CHECK_ERROR(ctx);

    properties.rw_use = 1;
    properties.rw = (4294967295 - 3);	// Special value meaning "ALL"
    error = riack_delete(client, cache->bucket, key, &properties);

    _riak_release_connection(tile, cache, client);

    if (error != RIACK_SUCCESS) {
        ctx->set_error(ctx, 500, "riak: failed to delete key %s: %d", key, error);
    }
}

/**
 * \brief get content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored on the riak server
 * \private \memberof mapcache_cache_riak
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_riak_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    int error;
    int connect_error = RIACK_SUCCESS;
    int retries = 3;
    RIACK_STRING key;
    struct RIACK_GET_OBJECT obj;
    struct RIACK_GET_PROPERTIES properties;
    struct RIACK_CLIENT *client;

    int took;

    apr_time_t start = apr_time_now();

    memset(&properties, 0, sizeof(struct RIACK_GET_PROPERTIES));

	//Use Buckets defaults instead of setting the read/write attributes
    /*
	properties.r_use = 1;
    properties.r = 1;
	*/

    mapcache_cache_riak *cache = (mapcache_cache_riak*)pcache;

    key.value = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FAILURE;
    }
    key.len = strlen(key.value);

    tile->encoded_data = mapcache_buffer_create(0, ctx->pool);

    client = _riak_get_connection(ctx, cache, tile);
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FAILURE;
    }

    // If we get an error it is advised that we call reconnect.  It also appears
    // that every now and then we get an error and need to retry once again to
    // get it to work.
    do
    {
        error = riack_get(client, cache->bucket, key, &properties, &obj);
        if (error != RIACK_SUCCESS) {
            ctx->log(ctx, MAPCACHE_WARN, "Retry %d in riak_get for tile %s from cache %s due to error %d", (4-retries), key.value, cache->cache.name, error);
            for (connect_error = riack_reconnect(client);
                 connect_error != RIACK_SUCCESS && retries > 0;
                 connect_error = riack_reconnect(client))
            {
              --retries;
            }

            --retries;
        }
    }
    while (error != RIACK_SUCCESS && retries >= 0);

    if (error != RIACK_SUCCESS)
    {
        if (connect_error != RIACK_SUCCESS)
            _riak_invalidate_connection(tile, cache, client);
        else
            _riak_release_connection(tile, cache, client);

        ctx->set_error(ctx, 500, "Failed to get tile %s from cache %s due to error %d", key.value, cache->cache.name, error);
        ctx->log(ctx, MAPCACHE_ERROR, "Failed to get tile %s from cache %s due to error %d", key.value, cache->cache.name, error);

        return MAPCACHE_FAILURE;
    }

    // Check if tile exists.  If it doesn't we need to return CACHE_MISS or things go wrong.
    // Mapcache doesn't appear to use the has_tile function and uses _get instead so we need
    // to do this sort of test here instead of erroring.
    if (obj.object.content_count < 1 || obj.object.content[0].data_len == 0) {
        riack_free_get_object(client, &obj);  // Need to free the object here as well.
        _riak_release_connection(tile, cache, client);
        return MAPCACHE_CACHE_MISS;
    }

    // Copy the data into the buffer
    mapcache_buffer_append(tile->encoded_data, obj.object.content[0].data_len, obj.object.content[0].data);

    riack_free_get_object(client, &obj);    // riack_get allocates the returned object so we need to deallocate it.

    apr_time_t now = apr_time_now();
    tile->mtime = now;

    _riak_release_connection(tile, cache, client); // Release the connection now that all memory is cleaned up.

    took = apr_time_as_msec(now - start);

    if ( took >= 100 ) {
	    ctx->log(ctx, MAPCACHE_DEBUG, "Get of tile %s took %d msecs", key.value, took);
    }

    return MAPCACHE_SUCCESS;
}

/**
 * \brief push tile data to riak
 *
 * writes the content of mapcache_tile::data to the configured riak instance(s)
 * \private \memberof mapcache_cache_riak
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_riak_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    char *key;
    int error;
    int connect_error = RIACK_SUCCESS;
    int retries = 3;
    struct RIACK_OBJECT object;
    struct RIACK_CONTENT content;
    struct RIACK_PUT_PROPERTIES properties;
    struct RIACK_CLIENT *client;

    int took;

    apr_time_t start = apr_time_now();

    memset(&content, 0, sizeof(struct RIACK_CONTENT));
    memset(&object, 0, sizeof(struct RIACK_OBJECT));
    memset(&properties, 0, sizeof(struct RIACK_PUT_PROPERTIES));

	//Use Buckets defaults instead of setting the read/write attributes
	/* 
    properties.w_use = 1;
    properties.w = 1;

    properties.dw_use = 1;
    properties.dw = 0;*/

    mapcache_cache_riak *cache = (mapcache_cache_riak*)pcache;

    key = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b", "#");
    GC_CHECK_ERROR(ctx);

    if (!tile->encoded_data) {
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        GC_CHECK_ERROR(ctx);
    }

    client = _riak_get_connection(ctx, cache, tile);
    GC_CHECK_ERROR(ctx);

    // Set up the riak object to put.  Need to do this after we get the client connection
    object.bucket.value = cache->bucket.value;
    object.bucket.len = cache->bucket.len;
    object.key.value = key;
    object.key.len = strlen(key);
    object.vclock.len = 0;
    object.content_count = 1;
    object.content = &content;
    content.content_type.value = tile->tileset->format->mime_type;
    content.content_type.len = strlen(tile->tileset->format->mime_type);
    content.data = (uint8_t*)tile->encoded_data->buf;
    content.data_len = tile->encoded_data->size;

    // If we get an error it is advised that we call reconnect.  It also appears
    // that every now and then we get an error and need to retry once again to
    // get it to work.
    do
    {
        error = riack_put(client, object, 0, &properties);
        if (error != RIACK_SUCCESS) {
            ctx->log(ctx, MAPCACHE_WARN, "Retry %d in riak_set for tile %s from cache %s due to eror %d", (4 - retries), key, cache->cache.name, error);
            for (connect_error = riack_reconnect(client);
                 connect_error != RIACK_SUCCESS && retries > 0;
                 connect_error = riack_reconnect(client))
            {
                --retries;
            }

            --retries;
        }
    }
    while (error != RIACK_SUCCESS && retries >= 0);

    if (connect_error != RIACK_SUCCESS)
        _riak_invalidate_connection(tile, cache, client);
    else
        _riak_release_connection(tile, cache, client);

    if (error != RIACK_SUCCESS)
    {
        ctx->set_error(ctx, 500, "failed to store tile %s to cache %s due to error %d.", key, cache->cache.name, error);
        ctx->log(ctx, MAPCACHE_ERROR, "Failed to set tile %s to cache %s due to error %d", key, cache->cache.name, error);
    }

    apr_time_t end = apr_time_now();

    took = apr_time_as_msec( end - start );

    if ( took >= 100 ) {
	ctx->log(ctx, MAPCACHE_DEBUG, "Set of tile %s took %d msecs", key, took );
    }
}

/**
 * \private \memberof mapcache_cache_riak
 */
static void _mapcache_cache_riak_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
    ezxml_t cur_node;
    mapcache_cache_riak *dcache = (mapcache_cache_riak*)cache;
    int servercount = 0;

    for (cur_node = ezxml_child(node,"server"); cur_node; cur_node = cur_node->next) {
        servercount++;
    }

    if (!servercount) {
        ctx->set_error(ctx, 400, "riak cache %s has no <server>s configured", cache->name);
        return;
    }

    if (servercount > 1) {
        ctx->set_error(ctx, 400, "riak cache %s has more than 1 server configured", cache->name);
        return;
    }

    cur_node = ezxml_child(node, "server");
    ezxml_t xhost = ezxml_child(cur_node, "host");   /* Host should contain just server */
    ezxml_t xport = ezxml_child(cur_node, "port");
    ezxml_t xbucket = ezxml_child(cur_node, "bucket");

    if (!xhost || !xhost->txt || ! *xhost->txt) {
        ctx->set_error(ctx, 400, "cache %s: <server> with no <host>", cache->name);
        return;
    } else {
        dcache->host = apr_pstrdup(ctx->pool, xhost->txt);
        if (dcache->host == NULL) {
            ctx->set_error(ctx, 400, "cache %s: failed to allocate host string!", cache->name);
            return;
        }
    }

    if (!xport || !xport->txt || ! *xport->txt) {
        ctx->set_error(ctx, 400, "cache %s: <server> with no <port>", cache->name);
        return;
    } else {
        dcache->port = atoi(xport->txt);
    }

    if (!xbucket || !xbucket->txt || ! *xbucket->txt) {
        ctx->set_error(ctx, 400, "cache %s: <server> with no <bucket>", cache->name);
        return;
    } else {
        dcache->bucket.value = apr_pstrdup(ctx->pool, xbucket->txt);
        if (dcache->bucket.value == NULL) {
            ctx->set_error(ctx, 400, "cache %s: failed to allocate bucket string!", cache->name);
            return;
        }
        dcache->bucket.len = strlen(dcache->bucket.value);
    }

    dcache->ctx = ctx;
}

/**
 * \private \memberof mapcache_cache_riak
 */
static void _mapcache_cache_riak_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache, mapcache_cfg *cfg) {
    riack_init();
}

/**
 * \brief creates and initializes a mapcache_riak_cache
 */
mapcache_cache* mapcache_cache_riak_create(mapcache_context *ctx) {
    mapcache_cache_riak *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_riak));
    if (!cache) {
        ctx->set_error(ctx, 500, "failed to allocate riak cache");
        return NULL;
    }

    cache->cache.metadata = apr_table_make(ctx->pool, 3);
    cache->cache.type = MAPCACHE_CACHE_RIAK;
    cache->cache.tile_get = _mapcache_cache_riak_get;
    cache->cache.tile_exists = _mapcache_cache_riak_has_tile;
    cache->cache.tile_set = _mapcache_cache_riak_set;
    cache->cache.tile_delete = _mapcache_cache_riak_delete;
    cache->cache.configuration_parse_xml = _mapcache_cache_riak_configuration_parse_xml;
    cache->cache.configuration_post_config = _mapcache_cache_riak_configuration_post_config;
    cache->host = NULL;
    cache->port = 8087;	// Default RIAK port used for protobuf

    return (mapcache_cache*)cache;
}

#endif
