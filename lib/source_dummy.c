/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Mapserver Mapfile datasource
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
#include "ezxml.h"
#include <apr_tables.h>
#include <apr_strings.h>

typedef struct mapcache_source_dummy mapcache_source_dummy;
struct mapcache_source_dummy {
  mapcache_source source;
  char *mapfile;
  void *mapobj;
};

/**
 * \private \memberof mapcache_source_dummy
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_dummy_render_map(mapcache_context *ctx, mapcache_source *psource, mapcache_map *map)
{
  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  memset(map->raw_image->data,255,map->width*map->height*4);
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
}

void _mapcache_source_dummy_query(mapcache_context *ctx, mapcache_source *psource, mapcache_feature_info *fi)
{
  ctx->set_error(ctx,500,"dummy source does not support queries");
}

/**
 * \private \memberof mapcache_source_dummy
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_dummy_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_source *source, mapcache_cfg *config)
{
}

/**
 * \private \memberof mapcache_source_dummy
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_dummy_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
}

mapcache_source* mapcache_source_dummy_create(mapcache_context *ctx)
{
  mapcache_source_dummy *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_dummy));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate dummy source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->source.type = MAPCACHE_SOURCE_DUMMY;
  source->source._render_map = _mapcache_source_dummy_render_map;
  source->source.configuration_check = _mapcache_source_dummy_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_dummy_configuration_parse_xml;
  source->source._query_info = _mapcache_source_dummy_query;
  return (mapcache_source*)source;
}


/* vim: ts=2 sts=2 et sw=2
*/
