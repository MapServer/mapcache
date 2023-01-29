/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: fallback cache backend.
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without fallbackriction, including without limitation
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

typedef struct mapcache_cache_fallback mapcache_cache_fallback;

struct mapcache_cache_fallback {
  mapcache_cache cache;
  apr_array_header_t *caches;
};

static int _mapcache_cache_fallback_tile_exists(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  mapcache_cache *subcache = APR_ARRAY_IDX(cache->caches,0,mapcache_cache*);
  return mapcache_cache_tile_exists(ctx, subcache, tile);
}

static void _mapcache_cache_fallback_tile_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  int i;
  for(i=0; i<cache->caches->nelts; i++) {
    mapcache_cache *subcache = APR_ARRAY_IDX(cache->caches,i,mapcache_cache*);
    mapcache_cache_tile_delete(ctx, subcache, tile);
    ctx->clear_errors(ctx); /* ignore errors */
  }
}

/**
 * \brief get content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored on the fallback server
 * \private \memberof mapcache_cache_fallback
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_fallback_tile_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  mapcache_cache *subcache;
  int i,ret;
  subcache = APR_ARRAY_IDX(cache->caches,0,mapcache_cache*);
  ret = mapcache_cache_tile_get(ctx, subcache, tile);
  
  if(ret == MAPCACHE_FAILURE) {
    int first_error = ctx->get_error(ctx);
    char *first_error_message = ctx->get_error_message(ctx);
    ctx->log(ctx,MAPCACHE_DEBUG,"failed \"GET\" on primary cache \"%s\" for tile (z=%d,x=%d,y=%d) of tileset \"%s\". Falling back on secondary caches",
            APR_ARRAY_IDX(cache->caches,0,mapcache_cache*)->name,tile->z,tile->x,tile->y,tile->tileset->name);
    ctx->clear_errors(ctx);
    for(i=1; i<cache->caches->nelts; i++) {
      subcache = APR_ARRAY_IDX(cache->caches,i,mapcache_cache*);
      if((ret = mapcache_cache_tile_get(ctx, subcache, tile)) == MAPCACHE_FAILURE) {
        ctx->log(ctx,MAPCACHE_DEBUG,"failed \"GET\" on fallback cache \"%s\" for tile (z=%d,x=%d,y=%d) of tileset \"%s\". Continuing with other fallback caches if available",
                APR_ARRAY_IDX(cache->caches,0,mapcache_cache*)->name,tile->z,tile->x,tile->y,tile->tileset->name);
        ctx->clear_errors(ctx);
        continue;
      } else {
        return ret;
      }
    }
    /* all backends failed, return primary error message */
    ctx->set_error(ctx,first_error,"%s",first_error_message);
    return MAPCACHE_FAILURE;
  } else {
    /* success or notfound */
    return ret;
  }
}

static void _mapcache_cache_fallback_tile_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  int i,first_error=0;
  char *first_error_message;
  for(i=0; i<cache->caches->nelts; i++) {
    mapcache_cache *subcache = APR_ARRAY_IDX(cache->caches,i,mapcache_cache*);
    mapcache_cache_tile_set(ctx, subcache, tile);
    if(GC_HAS_ERROR(ctx)) {
      if(!first_error) {
        first_error = ctx->get_error(ctx);
        first_error_message = ctx->get_error_message(ctx);
      }
      ctx->log(ctx,MAPCACHE_DEBUG,"failed \"SET\" on subcache \"%s\" for tile (z=%d,x=%d,y=%d) of tileset \"%s\"",
              APR_ARRAY_IDX(cache->caches,i,mapcache_cache*)->name,tile->z,tile->x,tile->y,tile->tileset->name);
      ctx->clear_errors(ctx);
    }
  }
  if(first_error) {
    ctx->set_error(ctx,first_error,"%s",first_error_message);
  }
}

static void _mapcache_cache_fallback_tile_multi_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  int i,first_error=0;
  char *first_error_message;
  for(i=0; i<cache->caches->nelts; i++) {
    mapcache_cache *subcache = APR_ARRAY_IDX(cache->caches,i,mapcache_cache*);
    mapcache_cache_tile_multi_set(ctx, subcache, tiles, ntiles);
    if(GC_HAS_ERROR(ctx)) {
      if(!first_error) {
        first_error = ctx->get_error(ctx);
        first_error_message = ctx->get_error_message(ctx);
      }
      ctx->log(ctx,MAPCACHE_DEBUG,"failed \"MULTISET\" on subcache \"%s\" for tile (z=%d,x=%d,y=%d) of tileset \"%s\"",
              APR_ARRAY_IDX(cache->caches,i,mapcache_cache*)->name,tiles[0].z,tiles[0].x,tiles[0].y,tiles[0].tileset->name);
      ctx->clear_errors(ctx);
    }
  }
  if(first_error) {
    ctx->set_error(ctx,first_error,"%s",first_error_message);
  }
}

/**
 * \private \memberof mapcache_cache_fallback
 */
static void _mapcache_cache_fallback_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *pcache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_fallback *cache = (mapcache_cache_fallback*)pcache;
  cache->caches = apr_array_make(ctx->pool,3,sizeof(mapcache_cache*));
  for(cur_node = ezxml_child(node,"cache"); cur_node; cur_node = cur_node->next) {
    mapcache_cache *refcache = mapcache_configuration_get_cache(config, cur_node->txt);
    if(!refcache) {
      ctx->set_error(ctx, 400, "fallback cache \"%s\" references cache \"%s\","
                     " but it is not configured (hint:referenced caches must be declared before this fallback cache in the xml file)", pcache->name, cur_node->txt);
      return;
    }
    APR_ARRAY_PUSH(cache->caches,mapcache_cache*) = refcache;
  }
  if(cache->caches->nelts == 0) {
    ctx->set_error(ctx,400,"fallback cache \"%s\" does not reference any child caches", pcache->name);
  }
}

/**
 * \private \memberof mapcache_cache_fallback
 */
static void _mapcache_cache_fallback_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
}


/**
 * \brief creates and initializes a mapcache_cache_fallback
 */
mapcache_cache* mapcache_cache_fallback_create(mapcache_context *ctx)
{
  mapcache_cache_fallback *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_fallback));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate fallback cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_COMPOSITE;
  cache->cache._tile_delete = _mapcache_cache_fallback_tile_delete;
  cache->cache._tile_get = _mapcache_cache_fallback_tile_get;
  cache->cache._tile_exists = _mapcache_cache_fallback_tile_exists;
  cache->cache._tile_set = _mapcache_cache_fallback_tile_set;
  cache->cache._tile_multi_set = _mapcache_cache_fallback_tile_multi_set;
  cache->cache.configuration_post_config = _mapcache_cache_fallback_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_fallback_configuration_parse_xml;
  cache->cache.child_init = mapcache_cache_child_init_noop;
  return (mapcache_cache*)cache;
}

