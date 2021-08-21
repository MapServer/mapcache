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

#ifdef USE_MAPSERVER
#include <apr_tables.h>
#include <apr_strings.h>
#ifdef APR_HAS_THREADS
#include <apr_thread_mutex.h>
#endif
#include <apr_hash.h>
#include <apr_reslist.h>
#include <mapserver.h>

/**\class mapcache_source_mapserver
 * \brief WMS mapcache_source
 * \implements mapcache_source
 */
typedef struct mapcache_source_mapserver mapcache_source_mapserver;
struct mapcache_source_mapserver {
  mapcache_source source;
  char *mapfile;
};

struct mc_mapobj {
  mapObj *map;
  mapcache_grid_link *grid_link;
  char *error;
};

void mapcache_mapserver_connection_constructor(mapcache_context *ctx, void **conn_, void *params) {
  mapcache_source_mapserver *src = (mapcache_source_mapserver*) params;
  struct mc_mapobj *mcmap = calloc(1,sizeof(struct mc_mapobj));
  mcmap->map = msLoadMap(src->mapfile,NULL);
  if(!mcmap->map) {
    errorObj *errors = NULL;
    ctx->set_error(ctx, 500, "Failed to load mapfile '%s'",src->mapfile);
    errors = msGetErrorObj();
    while(errors) {
      ctx->set_error(ctx, 500, "Failed to load mapfile '%s'. Mapserver reports: %s",src->mapfile, errors->message);
      errors = errors->next;
    }
    return;
  }
  msMapSetLayerProjections(mcmap->map);
  *conn_ = mcmap;
}

void mapcache_mapserver_connection_destructor(void *conn_) {
  struct mc_mapobj *mcmap = (struct mc_mapobj*) conn_;
  msFreeMap(mcmap->map);
  free(mcmap);
}

static mapcache_pooled_connection* _mapserver_get_connection(mapcache_context *ctx, mapcache_map *map)
{
  mapcache_pooled_connection *pc;
  char *key = apr_psprintf(ctx->pool, "ms_src_%s", map->tileset->source->name);

  pc = mapcache_connection_pool_get_connection(ctx, key, mapcache_mapserver_connection_constructor,
          mapcache_mapserver_connection_destructor, map->tileset->source);
  if(!GC_HAS_ERROR(ctx) && pc && pc->connection) {
  }

  return pc;
}


/**
 * \private \memberof mapcache_source_mapserver
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_mapserver_render_map(mapcache_context *ctx, mapcache_map *map)
{
  errorObj *errors = NULL;
  mapcache_pooled_connection *pc;
  struct mc_mapobj *mcmap;
  double dx, dy;
  rasterBufferObj rb;
  imageObj *image;

  pc = _mapserver_get_connection(ctx, map);
  GC_CHECK_ERROR(ctx);

  mcmap = pc->connection;
  GC_CHECK_ERROR(ctx);

  if(mcmap->grid_link != map->grid_link) {
    if (msLoadProjectionString(&(mcmap->map->projection), map->grid_link->grid->srs) != 0) {
      ctx->set_error(ctx,500, "Unable to set projection on mapObj.");
      errors = msGetErrorObj();
      while(errors) {
        ctx->set_error(ctx,500, "Unable to set projection on mapObj. MapServer reports: %s", errors->message);
        errors = errors->next;
      }
      mapcache_connection_pool_invalidate_connection(ctx,pc);
      return;
    }
    switch(map->grid_link->grid->unit) {
      case MAPCACHE_UNIT_DEGREES:
        mcmap->map->units = MS_DD;
        break;
      case MAPCACHE_UNIT_FEET:
        mcmap->map->units = MS_FEET;
        break;
      case MAPCACHE_UNIT_METERS:
        mcmap->map->units = MS_METERS;
        break;
    }
    mcmap->grid_link = map->grid_link;
  }


  /*
  ** WMS extents are edge to edge while MapServer extents are center of
  ** pixel to center of pixel.  Here we try to adjust the WMS extents
  ** in by half a pixel.
  */
  dx = (map->extent.maxx - map->extent.minx) / (map->width*2);
  dy = (map->extent.maxy - map->extent.miny) / (map->height*2);

  mcmap->map->extent.minx = map->extent.minx + dx;
  mcmap->map->extent.miny = map->extent.miny + dy;
  mcmap->map->extent.maxx = map->extent.maxx - dx;
  mcmap->map->extent.maxy = map->extent.maxy - dy;
  msMapSetSize(mcmap->map, map->width, map->height);

  image = msDrawMap(mcmap->map, MS_FALSE);
  if(!image) {
    ctx->set_error(ctx,500, "MapServer failed to create image.");
    errors = msGetErrorObj();
    while(errors) {
      ctx->set_error(ctx,500, "MapServer reports: %s", errors->message);
      errors = errors->next;
    }
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return;
  }

  if(image->format->vtable->supports_pixel_buffer) {
    if( MS_SUCCESS != image->format->vtable->getRasterBufferHandle(image,&rb)) {
      ctx->set_error(ctx,500,"failed to get mapserver raster buffer handle");
      mapcache_connection_pool_invalidate_connection(ctx,pc);
      return;
    }
  } else {
    ctx->set_error(ctx,500,"format %s has no pixel export",image->format->name);
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return;
  }

  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = 4 * map->width;
  map->raw_image->data = malloc(map->width*map->height*4);
  memcpy(map->raw_image->data,rb.data.rgba.pixels,map->width*map->height*4);
  apr_pool_cleanup_register(ctx->pool, map->raw_image->data,(void*)free, apr_pool_cleanup_null);
  msFreeImage(image);
  mapcache_connection_pool_release_connection(ctx,pc);

}

void _mapcache_source_mapserver_query(mapcache_context *ctx, mapcache_source *psource, mapcache_feature_info *fi)
{
  ctx->set_error(ctx,500,"mapserver source does not support queries");
}

/**
 * \private \memberof mapcache_source_mapserver
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_mapserver_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_source *source)
{
  ezxml_t cur_node;
  mapcache_source_mapserver *src = (mapcache_source_mapserver*)source;
  if ((cur_node = ezxml_child(node,"mapfile")) != NULL) {
    src->mapfile = apr_pstrdup(ctx->pool,cur_node->txt);
  }
}

/**
 * \private \memberof mapcache_source_mapserver
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_mapserver_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
  mapcache_source_mapserver *src = (mapcache_source_mapserver*)source;
  mapObj *map;
  /* check all required parameters are configured */
  if(!src->mapfile) {
    ctx->set_error(ctx, 400, "mapserver source %s has no <mapfile> configured",source->name);
  }
  if(!src->mapfile) {
    ctx->set_error(ctx,400,"mapserver source \"%s\" has no mapfile configured",src->source.name);
    return;
  }

  msSetup();

  /* do a test load to check the mapfile is correct */
  map = msLoadMap(src->mapfile, NULL);
  if(!map) {
    msWriteError(stderr);
    ctx->set_error(ctx,400,"failed to load mapfile \"%s\"",src->mapfile);
    return;
  }
  msFreeMap(map);
}

mapcache_source* mapcache_source_mapserver_create(mapcache_context *ctx)
{
  mapcache_source_mapserver *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_mapserver));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate mapserver source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->source.type = MAPCACHE_SOURCE_MAPSERVER;
  source->source.render_map = _mapcache_source_mapserver_render_map;
  source->source.configuration_check = _mapcache_source_mapserver_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_mapserver_configuration_parse_xml;
  source->source.query_info = _mapcache_source_mapserver_query;
  return (mapcache_source*)source;
}
#else
mapcache_source* mapcache_source_mapserver_create(mapcache_context *ctx)
{
  ctx->set_error(ctx, 500, "mapserver source not configured for this build");
  return NULL;
}
#endif

/* vim: ts=2 sts=2 et sw=2
*/
