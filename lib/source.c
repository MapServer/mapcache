/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: common datasource functions
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
#include <apr_time.h>

void mapcache_source_init(mapcache_context *ctx, mapcache_source *source)
{
  mapcache_extent tmp_extent = {-1,-1,-1,-1};
  source->data_extent = tmp_extent;
  source->metadata = apr_table_make(ctx->pool,3);
  source->retry_count = 1;
  source->retry_delay = 0.1;
}

void mapcache_source_render_map(mapcache_context *ctx, mapcache_source *source, mapcache_map *map) {
  int i;
#ifdef DEBUG
  ctx->log(ctx, MAPCACHE_DEBUG, "calling render_map on source (%s): tileset=%s, grid=%s, extent=(%f,%f,%f,%f)",
           source->name, map->tileset->name, map->grid_link->grid->name,
           map->extent.minx, map->extent.miny, map->extent.maxx, map->extent.maxy);
#endif
  for(i=0;i<=source->retry_count;i++) {
    if(i) { /* not our first try */
      ctx->log(ctx, MAPCACHE_INFO, "source (%s) render_map retry %d of %d. previous try returned error: %s",
               source->name, i, source->retry_count, ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(source->retry_delay > 0) {
        double wait = source->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    source->_render_map(ctx, source, map);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
}

void mapcache_source_query_info(mapcache_context *ctx, mapcache_source *source, mapcache_feature_info *fi) {
  int i;
#ifdef DEBUG
  ctx->log(ctx, MAPCACHE_DEBUG, "calling query_info on source (%s): tileset=%s, grid=%s,",
           source->name, fi->map.tileset->name, fi->map.grid_link->grid->name);
#endif
  for(i=0;i<=source->retry_count;i++) {
    if(i) { /* not our first try */
      ctx->log(ctx, MAPCACHE_INFO, "source (%s) render_map retry %d of %d. previous try returned error: %s",
               source->name, i, source->retry_count, ctx->get_error_message(ctx));
      ctx->clear_errors(ctx);
      if(source->retry_delay > 0) {
        double wait = source->retry_delay;
        int j = 0;
        for(j=1;j<i;j++) /* sleep twice as long as before previous retry */
          wait *= 2;
        apr_sleep((int)(wait*1000000));  /* apr_sleep expects microseconds */
      }
    }
    source->_query_info(ctx, source, fi);
    if(!GC_HAS_ERROR(ctx))
      break;
  }
}
/* vim: ts=2 sts=2 et sw=2
*/
