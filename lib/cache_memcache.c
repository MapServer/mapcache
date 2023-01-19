/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: memcache cache backend.
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

#include "mapcache.h"
#ifdef USE_MEMCACHE

#include <apr_memcache.h>

typedef struct mapcache_cache_memcache mapcache_cache_memcache;
/**\class mapcache_cache_memcache
 * \brief a mapcache_cache on memcached servers
 * \implements mapcache_cache
 */

struct mapcache_cache_memcache_server {
    char* host;
    int port;
};

struct mapcache_cache_memcache {
  mapcache_cache cache;
  int nservers;
  struct mapcache_cache_memcache_server *servers;
  int detect_blank;
};

struct mapcache_memcache_conn_param {
  mapcache_cache_memcache *cache;
};

struct mapcache_memcache_pooled_connection {
  apr_memcache_t *memcache;
  apr_pool_t *pool;
};

void mapcache_memcache_connection_constructor(mapcache_context *ctx, void **conn_, void *params) {
  struct mapcache_memcache_conn_param *param = params;
  mapcache_cache_memcache *cache = param->cache;
  struct mapcache_memcache_pooled_connection *pc;
  int i;
  pc = calloc(1,sizeof(struct mapcache_memcache_pooled_connection));
  apr_pool_create(&pc->pool,NULL);
  if(APR_SUCCESS != apr_memcache_create(pc->pool, cache->nservers, 0, &(pc->memcache))) {
    ctx->set_error(ctx,500,"cache %s: failed to create memcache backend", cache->cache.name);
    return;
  }
  for(i=0; i<param->cache->nservers; i++) {
    apr_memcache_server_t *server;
    if(APR_SUCCESS != apr_memcache_server_create(pc->pool,cache->servers[i].host,cache->servers[i].port,4,5,50,10000,&server)) {
      ctx->set_error(ctx,500,"cache %s: failed to create server %s:%d",cache->cache.name,cache->servers[i].host,cache->servers[i].port);
      return;
    }
    if(APR_SUCCESS != apr_memcache_add_server(pc->memcache,server)) {
      ctx->set_error(ctx,500,"cache %s: failed to add server %s:%d",cache->cache.name,cache->servers[i].host,cache->servers[i].port);
      return;
    }
  }
  *conn_ = pc;
}

void mapcache_memcache_connection_destructor(void *conn_) {
  struct mapcache_memcache_pooled_connection *pc = conn_;
  apr_pool_destroy(pc->pool);
  free(pc);
}

static mapcache_pooled_connection* _mapcache_memcache_get_conn(mapcache_context *ctx,
        mapcache_cache_memcache *cache, mapcache_tile *tile) {
  mapcache_pooled_connection *pc;
  struct mapcache_memcache_conn_param param;

  param.cache = cache;

  pc = mapcache_connection_pool_get_connection(ctx,cache->cache.name, mapcache_memcache_connection_constructor, mapcache_memcache_connection_destructor, &param);
  return pc;
}

static void _mapcache_memcache_release_conn(mapcache_context *ctx, mapcache_pooled_connection *con) {
  mapcache_connection_pool_release_connection(ctx, con);
}

static int _mapcache_cache_memcache_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *key;
  char *tmpdata;
  int rv;
  size_t tmpdatasize;
  mapcache_cache_memcache *cache = (mapcache_cache_memcache*)pcache;
  mapcache_pooled_connection *pc;
  struct mapcache_memcache_pooled_connection *mpc;
  pc = _mapcache_memcache_get_conn(ctx,cache,tile);
  if(GC_HAS_ERROR(ctx))
    return MAPCACHE_FALSE;
  mpc = pc->connection;
  
  key = mapcache_util_get_tile_key(ctx, tile, NULL, " \r\n\t\f\e\a\b","#");
  if(GC_HAS_ERROR(ctx)) {
    rv = MAPCACHE_FALSE;
    goto cleanup;
  }
  rv = apr_memcache_getp(mpc->memcache,ctx->pool,key,&tmpdata,&tmpdatasize,NULL);
  if(rv != APR_SUCCESS) {
    rv = MAPCACHE_FALSE;
    goto cleanup;
  }
  if(tmpdatasize == 0) {
    rv = MAPCACHE_FALSE;
    goto cleanup;
  }
  rv = MAPCACHE_TRUE;
cleanup:
  _mapcache_memcache_release_conn(ctx,pc);
  return rv;
}

static void _mapcache_cache_memcache_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *key;
  int rv;
  char errmsg[120];
  mapcache_cache_memcache *cache = (mapcache_cache_memcache*)pcache;
  mapcache_pooled_connection *pc;
  struct mapcache_memcache_pooled_connection *mpc;
  pc = _mapcache_memcache_get_conn(ctx,cache,tile);
  GC_CHECK_ERROR(ctx);
  mpc = pc->connection;
  key = mapcache_util_get_tile_key(ctx, tile,NULL," \r\n\t\f\e\a\b","#");
  if(GC_HAS_ERROR(ctx)) goto cleanup;
  
  rv = apr_memcache_delete(mpc->memcache,key,0);
  if(rv != APR_SUCCESS && rv!= APR_NOTFOUND) {
    ctx->set_error(ctx,500,"memcache: failed to delete key %s: %s", key, apr_strerror(rv,errmsg,120));
    goto cleanup;
  }

cleanup:
  _mapcache_memcache_release_conn(ctx,pc);
}

/**
 * \brief get content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored on the memcache server
 * \private \memberof mapcache_cache_memcache
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_memcache_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *key;
  int rv;
  mapcache_cache_memcache *cache = (mapcache_cache_memcache*)pcache;
  mapcache_pooled_connection *pc;
  mapcache_buffer *encoded_data;
  struct mapcache_memcache_pooled_connection *mpc;
  pc = _mapcache_memcache_get_conn(ctx,cache,tile);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FAILURE;
  }
  mpc = pc->connection;
  key = mapcache_util_get_tile_key(ctx, tile,NULL," \r\n\t\f\e\a\b","#");
  if(GC_HAS_ERROR(ctx)) {
    rv = MAPCACHE_FAILURE;
    goto cleanup;
  }
  encoded_data = mapcache_buffer_create(0,ctx->pool);
  rv = apr_memcache_getp(mpc->memcache,ctx->pool,key,(char**)&encoded_data->buf,&encoded_data->size,NULL);
  if(rv != APR_SUCCESS) {
    rv = MAPCACHE_CACHE_MISS;
    goto cleanup;
  }
  if(encoded_data->size == 0) {
    ctx->set_error(ctx,500,"memcache cache returned 0-length data for tile %d %d %d\n",tile->x,tile->y,tile->z);
    rv = MAPCACHE_FAILURE;
    goto cleanup;
  }
  /* extract the tile modification time from the end of the data returned */
  memcpy(
    &tile->mtime,
    &(((char*)encoded_data->buf)[encoded_data->size-sizeof(apr_time_t)]),
    sizeof(apr_time_t));
  
  ((char*)encoded_data->buf)[encoded_data->size-sizeof(apr_time_t)]='\0';
  encoded_data->avail = encoded_data->size;
  encoded_data->size -= sizeof(apr_time_t);
  if(((char*)encoded_data->buf)[0] == '#' && encoded_data->size > 1) {
    tile->encoded_data = mapcache_empty_png_decode(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy ,encoded_data->buf,&tile->nodata);
  } else {
    tile->encoded_data = encoded_data;
  }
  rv = MAPCACHE_SUCCESS;
  
cleanup:
  _mapcache_memcache_release_conn(ctx,pc);
  
  return rv;
}

/**
 * \brief push tile data to memcached
 *
 * writes the content of mapcache_tile::data to the configured memcached instance(s)
 * \returns MAPCACHE_FAILURE if there is no data to write, or if the tile isn't locked
 * \returns MAPCACHE_SUCCESS if the tile has been successfully written
 * \private \memberof mapcache_cache_memcache
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_memcache_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *key, *data;
  int rv;
  /* set no expiration if not configured */
  int expires =0;
  apr_time_t now;
  mapcache_buffer *encoded_data = NULL;
  mapcache_cache_memcache *cache = (mapcache_cache_memcache*)pcache;
  mapcache_pooled_connection *pc;
  struct mapcache_memcache_pooled_connection *mpc;
  pc = _mapcache_memcache_get_conn(ctx,cache,tile);
  GC_CHECK_ERROR(ctx);
  mpc = pc->connection;
  key = mapcache_util_get_tile_key(ctx, tile,NULL," \r\n\t\f\e\a\b","#");
  if(GC_HAS_ERROR(ctx)) goto cleanup;
  
  if(tile->tileset->auto_expire)
    expires = tile->tileset->auto_expire;

  if(cache->detect_blank) {
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    if(mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      encoded_data = mapcache_buffer_create(5,ctx->pool);
      ((char*)encoded_data->buf)[0] = '#';
      memcpy(((char*)encoded_data->buf)+1,tile->raw_image->data,4);
      encoded_data->size = 5;
    }
  }
  if(!encoded_data) {
    if(!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      if(GC_HAS_ERROR(ctx)) goto cleanup;
    }
    encoded_data = tile->encoded_data;
  }

  /* concatenate the current time to the end of the memcache data so we can extract it out
   * when we re-get the tile */
  data = calloc(1,encoded_data->size+sizeof(apr_time_t));
  now = apr_time_now();
  apr_pool_cleanup_register(ctx->pool, data, (void*)free, apr_pool_cleanup_null);
  memcpy(data,encoded_data->buf,encoded_data->size);
  memcpy(&(data[encoded_data->size]),&now,sizeof(apr_time_t));

  rv = apr_memcache_set(mpc->memcache,key,data,encoded_data->size+sizeof(apr_time_t),expires,0);
  if(rv != APR_SUCCESS) {
    ctx->set_error(ctx,500,"failed to store tile %d %d %d to memcache cache %s",
                   tile->x,tile->y,tile->z,cache->cache.name);
    goto cleanup;
  }

cleanup:
  _mapcache_memcache_release_conn(ctx,pc);
}

/**
 * \private \memberof mapcache_cache_memcache
 */
static void _mapcache_cache_memcache_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  int i = 0;
  mapcache_cache_memcache *dcache = (mapcache_cache_memcache*)cache;
  for(cur_node = ezxml_child(node,"server"); cur_node; cur_node = cur_node->next) {
    dcache->nservers++;
  }
  if(!dcache->nservers) {
    ctx->set_error(ctx,400,"memcache cache %s has no <server>s configured",cache->name);
    return;
  }
  dcache->servers = apr_pcalloc(ctx->pool, dcache->nservers * sizeof(struct mapcache_cache_memcache_server));

  for(cur_node = ezxml_child(node,"server"); cur_node; cur_node = cur_node->next) {
    ezxml_t xhost = ezxml_child(cur_node,"host");
    ezxml_t xport = ezxml_child(cur_node,"port");
    if(!xhost || !xhost->txt || ! *xhost->txt) {
      ctx->set_error(ctx,400,"cache %s: <server> with no <host>",cache->name);
      return;
    } else {
      dcache->servers[i].host = apr_pstrdup(ctx->pool,xhost->txt);
    }

    if(!xport || !xport->txt || ! *xport->txt) {
      ctx->set_error(ctx,400,"cache %s: <server> with no <port>", cache->name);
      return;
    } else {
      char *endptr;
      int iport = (int)strtol(xport->txt,&endptr,10);
      if(*endptr != 0) {
        ctx->set_error(ctx,400,"failed to parse value %s for memcache cache %s", xport->txt,cache->name);
        return;
      }
      dcache->servers[i].port = iport;
    }
    i++;
  }
  
  dcache->detect_blank = 0;
  if ((cur_node = ezxml_child(node, "detect_blank")) != NULL) {
    if(!strcasecmp(cur_node->txt,"true")) {
      dcache->detect_blank = 1;
    }
  }
}

/**
 * \private \memberof mapcache_cache_memcache
 */
static void _mapcache_cache_memcache_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
  mapcache_cache_memcache *dcache = (mapcache_cache_memcache*)cache;
  if(!dcache->nservers) {
    ctx->set_error(ctx,400,"cache %s has no servers configured",cache->name);
  }
}


/**
 * \brief creates and initializes a mapcache_memcache_cache
 */
mapcache_cache* mapcache_cache_memcache_create(mapcache_context *ctx)
{
  mapcache_cache_memcache *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_memcache));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate memcache cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_MEMCACHE;
  cache->cache._tile_get = _mapcache_cache_memcache_get;
  cache->cache._tile_exists = _mapcache_cache_memcache_has_tile;
  cache->cache._tile_set = _mapcache_cache_memcache_set;
  cache->cache._tile_delete = _mapcache_cache_memcache_delete;
  cache->cache.configuration_post_config = _mapcache_cache_memcache_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_memcache_configuration_parse_xml;
  cache->cache.child_init = mapcache_cache_child_init_noop;
  return (mapcache_cache*)cache;
}

#else
mapcache_cache* mapcache_cache_memcache_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"MEMCACHE support not compiled in this version");
  return NULL;
}
#endif

/* vim: ts=2 sts=2 et sw=2
*/
