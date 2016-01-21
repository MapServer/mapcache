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

int mapcache_cache_tile_get(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_get on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->grid_link->grid->name,tile->tileset->name,tile->z,tile->x, tile->y);
#endif
  return cache->_tile_get(ctx,cache,tile);
}

void mapcache_cache_tile_delete(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_delete on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->grid_link->grid->name,tile->tileset->name,tile->z,tile->x, tile->y);
#endif
  if(tile->tileset->read_only)
    return;
  return cache->_tile_delete(ctx,cache,tile);
}

int mapcache_cache_tile_exists(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_exists on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->grid_link->grid->name,tile->tileset->name,tile->z,tile->x, tile->y);
#endif
  return cache->_tile_exists(ctx,cache,tile);
}

void mapcache_cache_tile_set(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tile) {
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_set on cache (%s): (tileset=%s, grid=%s, z=%d, x=%d, y=%d",cache->name,tile->grid_link->grid->name,tile->tileset->name,tile->z,tile->x, tile->y);
#endif
  return cache->_tile_set(ctx,cache,tile);
}

void mapcache_cache_tile_multi_set(mapcache_context *ctx, mapcache_cache *cache, mapcache_tile *tiles, int ntiles) {
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"calling tile_multi_set on cache (%s): (tileset=%s, grid=%s, first tile: z=%d, x=%d, y=%d",cache->name,tiles[0].grid_link->grid->name,tiles[0].tileset->name,
      tiles[0].z,tiles[0].x, tiles[0].y);
#endif
  if(cache->_tile_multi_set) {
    return cache->_tile_multi_set(ctx,cache,tiles,ntiles);
  } else {
    int i;
    for( i=0;i<ntiles;i++ ) {
      cache->_tile_set(ctx, cache, &tiles[i]);
      GC_CHECK_ERROR(ctx);
    }
  }
}
