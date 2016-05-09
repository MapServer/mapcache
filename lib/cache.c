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

mapcache_rule* mapcache_cache_get_rule(mapcache_tile *tile) {
  /* get rule for tile if available */
  apr_array_header_t *rules = tile->grid_link->rules;
  mapcache_rule *rule;

  if(!rules) {
    return NULL;
  }

  rule = APR_ARRAY_IDX(rules, tile->z, mapcache_rule*);
  return rule;
}

int mapcache_cache_is_visible_tile(mapcache_tile *tile, mapcache_rule* rule) {
  /* check if tile is within visible extent */
  if(!rule) {
    return MAPCACHE_TRUE;
  }

  if(!rule->visible_limits) {
    return MAPCACHE_TRUE;
  }

  if(tile->x < rule->visible_limits->minx || tile->y < rule->visible_limits->miny || 
     tile->x > rule->visible_limits->maxx || tile->y > rule->visible_limits->maxy) {
    return MAPCACHE_FALSE;
  }

  return MAPCACHE_TRUE;
}

int mapcache_cache_is_readonly_tile(mapcache_tile *tile, mapcache_rule* rule) {
  /* check if tile is tile readonly */
  if(!rule) {
    return MAPCACHE_FALSE;
  }

  if(rule->readonly) {
    return MAPCACHE_TRUE;
  }

  return MAPCACHE_FALSE;
}

int mapcache_cache_tile_get(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
  int i,rv;
  mapcache_rule *rule = mapcache_cache_get_rule(tile);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_get on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is outside visible extent, create a blank tile and return */
  if (mapcache_cache_is_visible_tile(tile, rule) == MAPCACHE_FALSE) {
     int tile_sx, tile_sy;
     tile_sx = tile->grid_link->grid->tile_sx;
     tile_sy = tile->grid_link->grid->tile_sy;
     tile->encoded_data = tile->tileset->format->create_empty_image(ctx, tile->tileset->format, tile_sx, tile_sy, rule->hidden_color);
     if(GC_HAS_ERROR(ctx)) {
       return MAPCACHE_FAILURE;
     }
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
  mapcache_rule *rule = mapcache_cache_get_rule(tile);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_delete on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is readonly, return */
  if (mapcache_cache_is_readonly_tile(tile, rule) == MAPCACHE_TRUE) {
    return;
  }

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
  mapcache_rule *rule = mapcache_cache_get_rule(tile);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_exists on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is outside visible limits return TRUE
     a blank tile will be returned on subsequent get call on cache */
  if (mapcache_cache_is_visible_tile(tile, rule) == MAPCACHE_FALSE) {
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
  mapcache_rule *rule = mapcache_cache_get_rule(tile);
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_set on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->tileset->name,tile->grid_link->grid->name,tile->z,tile->x, tile->y);
#endif

  /* if tile is readonly, return */
  if (mapcache_cache_is_readonly_tile(tile, rule) == MAPCACHE_TRUE) {
    return;
  }

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

  /* if any tile is readonly, return */
  for(i = 0; i < ntiles; i++) {
    mapcache_rule *rule = mapcache_cache_get_rule(tiles+i);
    if (mapcache_cache_is_readonly_tile(tiles+i, rule) == MAPCACHE_TRUE) {
      return;
    }
  }

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
