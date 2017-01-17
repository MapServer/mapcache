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

typedef struct mapcache_source_fallback mapcache_source_fallback;
struct mapcache_source_fallback {
  mapcache_source source;
  apr_array_header_t *sources;
};

/**
 * \private \memberof mapcache_source_fallback
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_fallback_render_map(mapcache_context *ctx, mapcache_source *psource, mapcache_map *map)
{
  mapcache_source_fallback *source = (mapcache_source_fallback*)psource;
  mapcache_source *subsource;
  int i;
  subsource = APR_ARRAY_IDX(source->sources,0,mapcache_source*);
  mapcache_source_render_map(ctx, subsource, map);
  
  if(GC_HAS_ERROR(ctx)) {
    int first_error = ctx->get_error(ctx);
    char *first_error_message = ctx->get_error_message(ctx);
    ctx->log(ctx,MAPCACHE_INFO,
        "failed render on primary source \"%s\" on tileset \"%s\". Falling back on secondary sources",
        subsource->name,map->tileset->name);
    ctx->clear_errors(ctx);
    for(i=1; i<source->sources->nelts; i++) {
      subsource = APR_ARRAY_IDX(source->sources,i,mapcache_source*);
      mapcache_source_render_map(ctx, subsource, map);
      if(GC_HAS_ERROR(ctx)) {
        ctx->log(ctx,MAPCACHE_INFO,
            "failed render on fallback source \"%s\" of tileset \"%s\". Continuing with other fallback sources if available",
            subsource->name,map->tileset->name);
        ctx->clear_errors(ctx);
        continue;
      } else {
        return;
      }
    }
    /* all backends failed, return primary error message */
    ctx->set_error(ctx,first_error,first_error_message);
    return;
  }
}

void _mapcache_source_fallback_query(mapcache_context *ctx, mapcache_source *psource, mapcache_feature_info *fi)
{
  mapcache_source_fallback *source = (mapcache_source_fallback*)psource;
  mapcache_source *subsource;
  int i;
  subsource = APR_ARRAY_IDX(source->sources,0,mapcache_source*);
  mapcache_source_query_info(ctx, subsource, fi);
  
  if(GC_HAS_ERROR(ctx)) {
    int first_error = ctx->get_error(ctx);
    char *first_error_message = ctx->get_error_message(ctx);
    ctx->log(ctx,MAPCACHE_INFO,
        "failed query_info on primary source \"%s\" on tileset \"%s\". Falling back on secondary sources",
        subsource->name,fi->map.tileset->name);
    ctx->clear_errors(ctx);
    for(i=1; i<source->sources->nelts; i++) {
      subsource = APR_ARRAY_IDX(source->sources,i,mapcache_source*);
      mapcache_source_query_info(ctx, subsource, fi);
      if(GC_HAS_ERROR(ctx)) {
        ctx->log(ctx,MAPCACHE_INFO,
            "failed query_info on fallback source \"%s\" of tileset \"%s\". Continuing with other fallback sources if available",
            subsource->name,fi->map.tileset->name);
        ctx->clear_errors(ctx);
        continue;
      } else {
        return;
      }
    }
    /* all backends failed, return primary error message */
    ctx->set_error(ctx,first_error,first_error_message);
    return;
  }
  ctx->set_error(ctx,500,"fallback source does not support queries");
}

/**
 * \private \memberof mapcache_source_fallback
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_fallback_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_source *psource, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_source_fallback *source = (mapcache_source_fallback*)psource;
  source->sources = apr_array_make(ctx->pool,3,sizeof(mapcache_source*));
  for(cur_node = ezxml_child(node,"source"); cur_node; cur_node = cur_node->next) {
    mapcache_source *refsource = mapcache_configuration_get_source(config, cur_node->txt);
    if(!refsource) {
      ctx->set_error(ctx, 400, "fallback source \"%s\" references source \"%s\","
                     " but it is not configured (hint:referenced sources must be declared before this fallback source in the xml file)", psource->name, cur_node->txt);
      return;
    }
    APR_ARRAY_PUSH(source->sources,mapcache_source*) = refsource;
  }
  if(source->sources->nelts == 0) {
    ctx->set_error(ctx,400,"fallback source \"%s\" does not reference any child sources", psource->name);
  }
}

/**
 * \private \memberof mapcache_source_fallback
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_fallback_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
}

mapcache_source* mapcache_source_fallback_create(mapcache_context *ctx)
{
  mapcache_source_fallback *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_fallback));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate fallback source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->source.type = MAPCACHE_SOURCE_FALLBACK;
  source->source._render_map = _mapcache_source_fallback_render_map;
  source->source.configuration_check = _mapcache_source_fallback_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_fallback_configuration_parse_xml;
  source->source._query_info = _mapcache_source_fallback_query;
  return (mapcache_source*)source;
}


/* vim: ts=2 sts=2 et sw=2
*/
