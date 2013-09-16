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

/* hash table key = source->name, value = apr_reslist_t of mapObjs */
static apr_hash_t *mapobj_container = NULL;

struct mc_mapobj {
  mapObj *map;
  mapcache_grid_link *grid_link;
  char *error;
};

static apr_status_t _ms_get_mapobj(void **conn_, void *params, apr_pool_t *pool)
{
  mapcache_source_mapserver *src = (mapcache_source_mapserver*) params;
  struct mc_mapobj *mcmap = calloc(1,sizeof(struct mc_mapobj));
  *conn_ = mcmap;
  mcmap->map = msLoadMap(src->mapfile,NULL);
  if(!mcmap->map) {
    errorObj *errors = NULL;
    msWriteError(stderr);
    errors = msGetErrorObj();
    mcmap->error = apr_psprintf(pool,"Failed to load mapfile '%s'. Mapserver reports: %s",src->mapfile, errors->message);
    return APR_EGENERAL;
  }
  msMapSetLayerProjections(mcmap->map);
  return APR_SUCCESS;
}

static apr_status_t _ms_free_mapobj(void *conn_, void *params, apr_pool_t *pool)
{
  struct mc_mapobj *mcmap = (struct mc_mapobj*) conn_;
  msFreeMap(mcmap->map);
  free(mcmap);
  return APR_SUCCESS;
}

static struct mc_mapobj* _get_mapboj(mapcache_context *ctx, mapcache_map *map) {
  apr_status_t rv;
  mapcache_source_mapserver *src = (mapcache_source_mapserver*) map->tileset->source;
  struct mc_mapobj *mcmap;
  apr_reslist_t *mapobjs = NULL;
  if(!mapobj_container || NULL == (mapobjs = apr_hash_get(mapobj_container,src->source.name,APR_HASH_KEY_STRING))) {
#ifdef APR_HAS_THREADS
    if(ctx->threadlock)
      apr_thread_mutex_lock((apr_thread_mutex_t*)ctx->threadlock);
#endif
    if(!mapobj_container) {
      mapobj_container = apr_hash_make(ctx->process_pool);
    }
    mapobjs = apr_hash_get(mapobj_container,src->source.name,APR_HASH_KEY_STRING);
    if(!mapobjs) {
      apr_status_t rv;
      rv = apr_reslist_create(&mapobjs,
                              0 /* min */,
                              1 /* soft max */,
                              30 /* hard max */,
                              6 * 1000000 /*6 seconds, ttl*/,
                              _ms_get_mapobj, /* resource constructor */
                              _ms_free_mapobj, /* resource destructor */
                              src, ctx->process_pool);
      if (rv != APR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to create mapobj connection pool for cache %s", src->source.name);
#ifdef APR_HAS_THREADS
        if(ctx->threadlock)
          apr_thread_mutex_unlock((apr_thread_mutex_t*)ctx->threadlock);
#endif
        return NULL;
      }
      apr_hash_set(mapobj_container,src->source.name,APR_HASH_KEY_STRING,mapobjs);
    }
    assert(mapobjs);
#ifdef APR_HAS_THREADS
    if(ctx->threadlock)
      apr_thread_mutex_unlock((apr_thread_mutex_t*)ctx->threadlock);
#endif
  }
  rv = apr_reslist_acquire(mapobjs, (void **) &mcmap);
  if (rv != APR_SUCCESS) {
    ctx->set_error(ctx, 500, "failed to aquire mappObj instance: %s", mcmap->error);
    return NULL;
  }
  return mcmap;
}

static void _release_mapboj(mapcache_context *ctx, mapcache_map *map, struct mc_mapobj *mcmap)
{
  mapcache_source_mapserver *src = (mapcache_source_mapserver*) map->tileset->source;
  msFreeLabelCache(&mcmap->map->labelcache);
  apr_reslist_t *mapobjs = apr_hash_get(mapobj_container,src->source.name, APR_HASH_KEY_STRING);
  assert(mapobjs);
  if (GC_HAS_ERROR(ctx)) {
    apr_reslist_invalidate(mapobjs, (void*) mcmap);
  } else {
    apr_reslist_release(mapobjs, (void*) mcmap);
  }
}
/**
 * \private \memberof mapcache_source_mapserver
 * \sa mapcache_source::render_map()
 */
void _mapcache_source_mapserver_render_map(mapcache_context *ctx, mapcache_map *map)
{
  errorObj *errors = NULL;

  struct mc_mapobj *mcmap = _get_mapboj(ctx,map);
  GC_CHECK_ERROR(ctx);

  if(mcmap->grid_link != map->grid_link) {
    if (msLoadProjectionString(&(mcmap->map->projection), map->grid_link->grid->srs) != 0) {
      errors = msGetErrorObj();
      ctx->set_error(ctx,500, "Unable to set projection on mapObj. MapServer reports: %s", errors->message);
      _release_mapboj(ctx,map,mcmap);
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
  double dx, dy;
  dx = (map->extent.maxx - map->extent.minx) / (map->width*2);
  dy = (map->extent.maxy - map->extent.miny) / (map->height*2);

  mcmap->map->extent.minx = map->extent.minx + dx;
  mcmap->map->extent.miny = map->extent.miny + dy;
  mcmap->map->extent.maxx = map->extent.maxx - dx;
  mcmap->map->extent.maxy = map->extent.maxy - dy;
  msMapSetSize(mcmap->map, map->width, map->height);

  imageObj *image = msDrawMap(mcmap->map, MS_FALSE);
  if(!image) {
    errors = msGetErrorObj();
    ctx->set_error(ctx,500, "MapServer failed to create image. MapServer reports: %s", errors->message);
    _release_mapboj(ctx,map,mcmap);
    return;
  }
  rasterBufferObj rb;

  if(image->format->vtable->supports_pixel_buffer) {
    image->format->vtable->getRasterBufferHandle(image,&rb);
  } else {
    ctx->set_error(ctx,500,"format %s has no pixel export",image->format->name);
    _release_mapboj(ctx,map,mcmap);
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
  _release_mapboj(ctx,map,mcmap);

}

void _mapcache_source_mapserver_query(mapcache_context *ctx, mapcache_feature_info *fi)
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
  mapObj *map = msLoadMap(src->mapfile, NULL);
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
}
#endif

/* vim: ts=2 sts=2 et sw=2
*/
