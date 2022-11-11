/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: generic cache access
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2015 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without compositeriction, including without limitation
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
#include <apr_time.h>

int mapcache_cache_tile_get(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
  int i,rv;
  mapcache_rule *rule = mapcache_ruleset_rule_get(tile->grid_link->rules, tile->z);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_get on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is outside visible limits, return a blank tile */
  if (mapcache_ruleset_is_visible_tile(rule, tile) == MAPCACHE_FALSE) {
    tile->encoded_data = mapcache_buffer_create(0, ctx->pool);
    mapcache_buffer_append(tile->encoded_data, rule->hidden_tile->size, rule->hidden_tile->buf);
    return MAPCACHE_SUCCESS;
  }

  for(i=0;i<=cache->retry_count;i++) {
    if(i) {
      ctx->log(ctx,MAPCACHE_INFO,"cache (%s) get retry %d of %d. previous try returned error: %s",cache->name,i,cache->retry_count,ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(cache->retry_delay > 0) {
        double wait = cache->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    rv = cache->_tile_get(ctx,cache,tile);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
  return rv;
}

void mapcache_cache_tile_delete(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
  int i;
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_delete on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif
  if(tile->tileset->read_only)
    return;
  for(i=0;i<=cache->retry_count;i++) {
    if(i) {
      ctx->log(ctx,MAPCACHE_INFO,"cache (%s) delete retry %d of %d. previous try returned error: %s",cache->name,i,cache->retry_count,ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(cache->retry_delay > 0) {
        double wait = cache->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    cache->_tile_delete(ctx,cache,tile);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
}

int mapcache_cache_tile_exists(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
  int i,rv;
  mapcache_rule *rule = mapcache_ruleset_rule_get(tile->grid_link->rules, tile->z);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_exists on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is outside visible limits return TRUE
     a blank tile will be returned on subsequent get call on cache */
  if (mapcache_ruleset_is_visible_tile(rule, tile) == MAPCACHE_FALSE) {
    return MAPCACHE_TRUE;
  }

  for(i=0;i<=cache->retry_count;i++) {
    if(i) {
      ctx->log(ctx,MAPCACHE_INFO,"cache (%s) exists retry %d of %d. previous try returned error: %s",cache->name,i,cache->retry_count,ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(cache->retry_delay > 0) {
        double wait = cache->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    rv = cache->_tile_exists(ctx,cache,tile);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
  return rv;
}

void mapcache_cache_tile_set(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
  int i;
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_set on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif
  if(tile->tileset->read_only)
    return;
  for(i=0;i<=cache->retry_count;i++) {
    if(i) {
      ctx->log(ctx,MAPCACHE_INFO,"cache (%s) set retry %d of %d. previous try returned error: %s",cache->name,i,cache->retry_count,ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(cache->retry_delay > 0) {
        double wait = cache->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    cache->_tile_set(ctx,cache,tile);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
}

void mapcache_cache_tile_multi_set(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tiles, int ntiles) {
  int i;
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_multi_set on cache (%s): (tileset=%s, grid=%s, first tile: z=%d, x=%d, y=%d",cache->name,tiles[0].tileset->name,tiles[0].grid_link->grid->name,
      tiles[0].z,tiles[0].x, tiles[0].y);
#endif
  if((&tiles[0])->tileset->read_only)
    return;
  if(cache->_tile_multi_set) {
    for(i=0;i<=cache->retry_count;i++) {
      if(i) {
        ctx->log(ctx,MAPCACHE_INFO,"cache (%s) multi-set retry %d of %d. previous try returned error: %s",cache->name,i,cache->retry_count,ctx->get_error_message(ctx));
        ctx->clear_errors(ctx);
        if(cache->retry_delay > 0) {
          double wait = cache->retry_delay;
          int j = 0;
          for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
            wait *= 2;
          apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
        }
      }
      cache->_tile_multi_set(ctx,cache,tiles,ntiles);
      if(!GC_HAS_ERROR(ctx))
        break;
    }
  } else {
    for( i=0;i<ntiles;i++ ) {
      mapcache_cache_tile_set(ctx, cache, tiles+i);
    }
  }
}

void mapcache_cache_child_init(mapcache_context *ctx, mapcache_cfg *config, apr_pool_t *pchild)
{
  apr_hash_index_t *cachei = apr_hash_first(pchild,config->caches);
  while(cachei) {
    mapcache_cache *cache;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(cachei,&key,&keylen,(void**)&cache);
    cache->child_init(ctx,cache,pchild);
    cachei = apr_hash_next(cachei);
  }
}
