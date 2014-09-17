/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: xml configuration parser
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
#include <string.h>
#include <stdlib.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_file_info.h>
#include <math.h>


void parseMetadata(mapcache_context *ctx, ezxml_t node, apr_table_t *metadata)
{
  ezxml_t cur_node;
  for(cur_node = node->child; cur_node; cur_node = cur_node->sibling) {
    apr_table_add(metadata,cur_node->name, cur_node->txt);
  }
}

void parseTimeDimension(mapcache_context *ctx, ezxml_t node, mapcache_tileset *tileset) {
  const char *attr = NULL;
  int delay;
  if(tileset->timedimension) {
    ctx->set_error(ctx,400,"tileset \"%s\" can only have a single <timedimension>", tileset->name);
    return;
  }
  attr = ezxml_attr(node,"type");
  if(attr && *attr) {
    if(!strcmp(attr,"sqlite")) {
#ifdef USE_SQLITE
      tileset->timedimension = mapcache_timedimension_sqlite_create(ctx->pool);
#else
      ctx->set_error(ctx,400, "failed to add sqlite timedimension: Sqlite support is not available on this build");
      return;
#endif
    } else {
      ctx->set_error(ctx,400,"unknown \"type\" attribute \"%s\" for %s's <timedimension>. Expecting one of (sqlite)",attr,tileset->name);
      return;
    }

  } else {
    ctx->set_error(ctx,400,"missing \"type\" attribute for %s's <timedimension>",tileset->name);
    return;
  }
  attr = ezxml_attr(node,"name");
  if(attr && *attr) {
    tileset->timedimension->key = apr_pstrdup(ctx->pool,attr);
  } else {
    tileset->timedimension->key = apr_pstrdup(ctx->pool,"TIME");
  }

  attr = ezxml_attr(node,"default");
  if(attr && *attr) {
    tileset->timedimension->default_value = apr_pstrdup(ctx->pool,attr);
  } else {
    ctx->set_error(ctx,400,"no \"default\" attribute for <timedimension> %s",tileset->timedimension->key);
    return;
  }

  attr = ezxml_attr(node, "animate");
  if (attr && *attr){
    if(!strcasecmp(attr, "true")) {
      tileset->timedimension->assembly_type = MAPCACHE_TIMEDIMENSION_ASSEMBLY_ANIMATE;
      attr = ezxml_attr(node, "delay");
      if (attr && *attr) {
        delay = atoi(attr);
        if (delay > 0)
          tileset->timedimension->delay = delay;
        else
          ctx->set_error(ctx,400, "the \"delay \" attribute for <timedimension> %s must be an integer bigger than 0", tileset->timedimension->key);
      }
    }
  } else {
  ctx->set_error(ctx,400,"no \"animate\" attribute for <timedimension> %s", tileset->timedimension->key);
}


  tileset->timedimension->configuration_parse_xml(ctx,tileset->timedimension,node);
}

void parseDimensions(mapcache_context *ctx, ezxml_t node, mapcache_tileset *tileset)
{
  ezxml_t dimension_node;
  apr_array_header_t *dimensions = apr_array_make(ctx->pool,1,sizeof(mapcache_dimension*));
  for(dimension_node = ezxml_child(node,"dimension"); dimension_node; dimension_node = dimension_node->next) {
    char *name = (char*)ezxml_attr(dimension_node,"name");
    char *type = (char*)ezxml_attr(dimension_node,"type");
    char *unit = (char*)ezxml_attr(dimension_node,"unit");
    char *default_value = (char*)ezxml_attr(dimension_node,"default");

    mapcache_dimension *dimension = NULL;

    if(!name || !strlen(name)) {
      ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <dimension>");
      return;
    }

    if(type && *type) {
      if(!strcmp(type,"values")) {
        dimension = mapcache_dimension_values_create(ctx->pool);
      } else if(!strcmp(type,"regex")) {
        dimension = mapcache_dimension_regex_create(ctx->pool);
      } else if(!strcmp(type,"intervals")) {
        dimension = mapcache_dimension_intervals_create(ctx->pool);
      } else if(!strcmp(type,"time")) {
        ctx->set_error(ctx,501,"time dimension type not implemented yet");
        return;
        dimension = mapcache_dimension_time_create(ctx->pool);
      } else {
        ctx->set_error(ctx,400,"unknown dimension type \"%s\"",type);
        return;
      }
    } else {
      ctx->set_error(ctx,400, "mandatory attribute \"type\" not found in <dimensions>");
      return;
    }

    dimension->name = apr_pstrdup(ctx->pool,name);

    if(unit && *unit) {
      dimension->unit = apr_pstrdup(ctx->pool,unit);
    }

    if(default_value && *default_value) {
      dimension->default_value = apr_pstrdup(ctx->pool,default_value);
    } else {
      ctx->set_error(ctx,400,"dimension \"%s\" has no \"default\" attribute",dimension->name);
      return;
    }

    dimension->configuration_parse_xml(ctx,dimension,dimension_node);
    GC_CHECK_ERROR(ctx);

    APR_ARRAY_PUSH(dimensions,mapcache_dimension*) = dimension;
  }
  if(apr_is_empty_array(dimensions)) {
    ctx->set_error(ctx, 400, "<dimensions> for tileset \"%s\" has no dimensions defined (expecting <dimension> children)",tileset->name);
    return;
  }
  tileset->dimensions = dimensions;
}

void parseGrid(mapcache_context *ctx, ezxml_t node, mapcache_cfg *config)
{
  char *name;
  mapcache_extent extent = {0,0,0,0};
  mapcache_grid *grid;
  ezxml_t cur_node;
  char *value;

  name = (char*)ezxml_attr(node,"name");
  if(!name || !strlen(name)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <grid>");
    return;
  } else {
    name = apr_pstrdup(ctx->pool, name);
    /* check we don't already have a grid defined with this name */
    if(mapcache_configuration_get_grid(config, name)) {
      ctx->set_error(ctx, 400, "duplicate grid with name \"%s\"",name);
      return;
    }
  }
  grid = mapcache_grid_create(ctx->pool);
  grid->name = name;

  if ((cur_node = ezxml_child(node,"extent")) != NULL) {
    double *values;
    int nvalues;
    value = apr_pstrdup(ctx->pool,cur_node->txt);
    if(MAPCACHE_SUCCESS != mapcache_util_extract_double_list(ctx, value, NULL, &values, &nvalues) ||
        nvalues != 4) {
      ctx->set_error(ctx, 400, "failed to parse extent array %s."
                     "(expecting 4 space separated numbers, got %d (%f %f %f %f)"
                     "eg <extent>-180 -90 180 90</extent>",
                     value,nvalues,values[0],values[1],values[2],values[3]);
      return;
    }
    extent.minx = values[0];
    extent.miny = values[1];
    extent.maxx = values[2];
    extent.maxy = values[3];
  }

  if ((cur_node = ezxml_child(node,"metadata")) != NULL) {
    parseMetadata(ctx, cur_node, grid->metadata);
    GC_CHECK_ERROR(ctx);
  }

  if ((cur_node = ezxml_child(node,"units")) != NULL) {
    if(!strcasecmp(cur_node->txt,"dd")) {
      grid->unit = MAPCACHE_UNIT_DEGREES;
    } else if(!strcasecmp(cur_node->txt,"m")) {
      grid->unit = MAPCACHE_UNIT_METERS;
    } else if(!strcasecmp(cur_node->txt,"ft")) {
      grid->unit = MAPCACHE_UNIT_FEET;
    } else {
      ctx->set_error(ctx, 400, "unknown unit %s for grid %s (valid values are \"dd\", \"m\", and \"ft\"",
                     cur_node->txt, grid->name);
      return;
    }
  }
  if ((cur_node = ezxml_child(node,"srs")) != NULL) {
    grid->srs = apr_pstrdup(ctx->pool,cur_node->txt);
  }

  for(cur_node = ezxml_child(node,"srsalias"); cur_node; cur_node = cur_node->next) {
    value = apr_pstrdup(ctx->pool,cur_node->txt);
    APR_ARRAY_PUSH(grid->srs_aliases,char*) = value;
  }

  if ((cur_node = ezxml_child(node,"origin")) != NULL) {
    if(!strcasecmp(cur_node->txt,"top-left")) {
      grid->origin = MAPCACHE_GRID_ORIGIN_TOP_LEFT;
    } else if(!strcasecmp(cur_node->txt,"bottom-left")) {
      grid->origin = MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT;
    } else if(!strcasecmp(cur_node->txt,"top-right")) {
      grid->origin = MAPCACHE_GRID_ORIGIN_TOP_RIGHT;
    } else if(!strcasecmp(cur_node->txt,"bottom-right")) {
      grid->origin = MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT;
    } else {
      ctx->set_error(ctx, 400,
          "unknown origin %s for grid %s (valid values are \"top-left\", \"bottom-left\", \"top-right\" and \"bottom-right\"",
          cur_node->txt, grid->name);
      return;
    }
    if(grid->origin == MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT || grid->origin == MAPCACHE_GRID_ORIGIN_TOP_RIGHT) {
      ctx->set_error(ctx,500,"grid origin %s not implemented",cur_node->txt);
      return;
    }
  }
  if ((cur_node = ezxml_child(node,"size")) != NULL) {
    int *sizes, nsizes;
    value = apr_pstrdup(ctx->pool,cur_node->txt);

    if(MAPCACHE_SUCCESS != mapcache_util_extract_int_list(ctx, value, NULL, &sizes, &nsizes) ||
        nsizes != 2) {
      ctx->set_error(ctx, 400, "failed to parse size array %s in  grid %s"
                     "(expecting two space separated integers, eg <size>256 256</size>",
                     value, grid->name);
      return;
    }
    grid->tile_sx = sizes[0];
    grid->tile_sy = sizes[1];
  }

  if ((cur_node = ezxml_child(node,"resolutions")) != NULL) {
    int nvalues;
    double *values;
    value = apr_pstrdup(ctx->pool,cur_node->txt);

    if(MAPCACHE_SUCCESS != mapcache_util_extract_double_list(ctx, value, NULL, &values, &nvalues) ||
        !nvalues) {
      ctx->set_error(ctx, 400, "failed to parse resolutions array %s."
                     "(expecting space separated numbers, "
                     "eg <resolutions>1 2 4 8 16 32</resolutions>",
                     value);
      return;
    }
    grid->nlevels = nvalues;
    grid->levels = (mapcache_grid_level**)apr_pcalloc(ctx->pool,
                   grid->nlevels*sizeof(mapcache_grid_level));
    while(nvalues--) {
      double unitheight;
      double unitwidth;
      mapcache_grid_level *level = (mapcache_grid_level*)apr_pcalloc(ctx->pool,sizeof(mapcache_grid_level));
      level->resolution = values[nvalues];
      unitheight = grid->tile_sy * level->resolution;
      unitwidth = grid->tile_sx * level->resolution;
      level->maxy = ceil((extent.maxy-extent.miny - 0.01* unitheight)/unitheight);
      level->maxx = ceil((extent.maxx-extent.minx - 0.01* unitwidth)/unitwidth);
      grid->levels[nvalues] = level;
    }
  }

  if(grid->srs == NULL) {
    ctx->set_error(ctx, 400, "grid \"%s\" has no srs configured."
                   " You must add a <srs> tag.", grid->name);
    return;
  }
  if(extent.minx >= extent.maxx || extent.miny >= extent.maxy) {
    ctx->set_error(ctx, 400, "grid \"%s\" has no (or invalid) extent configured"
                   " You must add/correct a <extent> tag.", grid->name);
    return;
  } else {
    grid->extent = extent;
  }
  if(grid->tile_sx <= 0 || grid->tile_sy <= 0) {
    ctx->set_error(ctx, 400, "grid \"%s\" has no (or invalid) tile size configured"
                   " You must add/correct a <size> tag.", grid->name);
    return;
  }
  if(!grid->nlevels) {
    ctx->set_error(ctx, 400, "grid \"%s\" has no resolutions configured."
                   " You must add a <resolutions> tag.", grid->name);
    return;
  }
  mapcache_configuration_add_grid(config,grid,name);
}

void parseSource(mapcache_context *ctx, ezxml_t node, mapcache_cfg *config)
{
  ezxml_t cur_node;
  char *name = NULL, *type = NULL;
  mapcache_source *source;

  name = (char*)ezxml_attr(node,"name");
  type = (char*)ezxml_attr(node,"type");
  if(!name || !strlen(name)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <source>");
    return;
  } else {
    name = apr_pstrdup(ctx->pool, name);
    /* check we don't already have a source defined with this name */
    if(mapcache_configuration_get_source(config, name)) {
      ctx->set_error(ctx, 400, "duplicate source with name \"%s\"",name);
      return;
    }
  }
  if(!type || !strlen(type)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"type\" not found in <source>");
    return;
  }
  source = NULL;
  if(!strcmp(type,"wms")) {
    source = mapcache_source_wms_create(ctx);
#ifdef USE_MAPSERVER
  } else if(!strcmp(type,"mapserver")) {
    source = mapcache_source_mapserver_create(ctx);
#endif
  } else if(!strcmp(type,"gdal")) {
    source = mapcache_source_gdal_create(ctx);
  } else if(!strcmp(type,"dummy")) {
    source = mapcache_source_dummy_create(ctx);
  } else {
    ctx->set_error(ctx, 400, "unknown source type %s for source \"%s\"", type, name);
    return;
  }
  if(source == NULL) {
    ctx->set_error(ctx, 400, "failed to parse source \"%s\"", name);
    return;
  }
  source->name = name;

  if ((cur_node = ezxml_child(node,"metadata")) != NULL) {
    parseMetadata(ctx, cur_node, source->metadata);
    GC_CHECK_ERROR(ctx);
  }

  source->configuration_parse_xml(ctx,node,source);
  GC_CHECK_ERROR(ctx);
  source->configuration_check(ctx,config,source);
  GC_CHECK_ERROR(ctx);
  mapcache_configuration_add_source(config,source,name);
}

void parseFormat(mapcache_context *ctx, ezxml_t node, mapcache_cfg *config)
{
  char *name = NULL,  *type = NULL;
  mapcache_image_format *format = NULL;
  ezxml_t cur_node;
  name = (char*)ezxml_attr(node,"name");
  type = (char*)ezxml_attr(node,"type");
  if(!name || !strlen(name)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <format>");
    return;
  }
  name = apr_pstrdup(ctx->pool, name);
  if(!type || !strlen(type)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"type\" not found in <format>");
    return;
  }
  if(!strcmp(type,"PNG")) {
    int colors = -1;
    mapcache_compression_type compression = MAPCACHE_COMPRESSION_DEFAULT;
    if ((cur_node = ezxml_child(node,"compression")) != NULL) {
      if(!strcmp(cur_node->txt, "fast")) {
        compression = MAPCACHE_COMPRESSION_FAST;
      } else if(!strcmp(cur_node->txt, "best")) {
        compression = MAPCACHE_COMPRESSION_BEST;
      } else if(!strcmp(cur_node->txt, "none")) {
        compression = MAPCACHE_COMPRESSION_DISABLE;
      } else {
        ctx->set_error(ctx, 400, "unknown compression type %s for format \"%s\"", cur_node->txt, name);
        return;
      }
    }
    if ((cur_node = ezxml_child(node,"colors")) != NULL) {
      char *endptr;
      colors = (int)strtol(cur_node->txt,&endptr,10);
      if(*endptr != 0 || colors < 2 || colors > 256) {
        ctx->set_error(ctx, 400, "failed to parse colors \"%s\" for format \"%s\""
                       "(expecting an  integer between 2 and 256 "
                       "eg <colors>256</colors>",
                       cur_node->txt,name);
        return;
      }
    }

    if(colors == -1) {
      format = mapcache_imageio_create_png_format(ctx->pool,
               name,compression);
    } else {
      format = mapcache_imageio_create_png_q_format(ctx->pool,
               name,compression, colors);
    }
  } else if(!strcmp(type,"JPEG")) {
    int quality = 95;
    mapcache_photometric photometric = MAPCACHE_PHOTOMETRIC_YCBCR;
    if ((cur_node = ezxml_child(node,"quality")) != NULL) {
      char *endptr;
      quality = (int)strtol(cur_node->txt,&endptr,10);
      if(*endptr != 0 || quality < 1 || quality > 100) {
        ctx->set_error(ctx, 400, "failed to parse quality \"%s\" for format \"%s\""
                       "(expecting an  integer between 1 and 100 "
                       "eg <quality>90</quality>",
                       cur_node->txt,name);
        return;
      }
    }
    if ((cur_node = ezxml_child(node,"photometric")) != NULL) {
      if(!strcasecmp(cur_node->txt,"RGB"))
        photometric = MAPCACHE_PHOTOMETRIC_RGB;
      else if(!strcasecmp(cur_node->txt,"YCBCR"))
        photometric = MAPCACHE_PHOTOMETRIC_YCBCR;
      else {
        ctx->set_error(ctx,500,"failed to parse jpeg format %s photometric %s. expecting rgb or ycbcr",
                       name,cur_node->txt);
        return;
      }
    }
    format = mapcache_imageio_create_jpeg_format(ctx->pool,
             name,quality,photometric);
  } else if(!strcasecmp(type,"MIXED")) {
    mapcache_image_format *transparent=NULL, *opaque=NULL;
    if ((cur_node = ezxml_child(node,"transparent")) != NULL) {
      transparent = mapcache_configuration_get_image_format(config,cur_node->txt);
    }
    if(!transparent) {
      ctx->set_error(ctx,400, "mixed format %s references unknown transparent format %s"
                     "(order is important, format %s should appear first)",
                     name,cur_node->txt,cur_node->txt);
      return;
    }
    if ((cur_node = ezxml_child(node,"opaque")) != NULL) {
      opaque = mapcache_configuration_get_image_format(config,cur_node->txt);
    }
    if(!opaque) {
      ctx->set_error(ctx,400, "mixed format %s references unknown opaque format %s"
                     "(order is important, format %s should appear first)",
                     name,cur_node->txt,cur_node->txt);
      return;
    }
    format = mapcache_imageio_create_mixed_format(ctx->pool,name,transparent, opaque);
  } else {
    ctx->set_error(ctx, 400, "unknown format type %s for format \"%s\"", type, name);
    return;
  }
  if(format == NULL) {
    ctx->set_error(ctx, 400, "failed to parse format \"%s\"", name);
    return;
  }


  mapcache_configuration_add_image_format(config,format,name);
  return;
}

void parseCache(mapcache_context *ctx, ezxml_t node, mapcache_cfg *config)
{
  char *name = NULL,  *type = NULL;
  mapcache_cache *cache = NULL;
  name = (char*)ezxml_attr(node,"name");
  type = (char*)ezxml_attr(node,"type");
  if(!name || !strlen(name)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <cache>");
    return;
  } else {
    name = apr_pstrdup(ctx->pool, name);
    /* check we don't already have a cache defined with this name */
    if(mapcache_configuration_get_cache(config, name)) {
      ctx->set_error(ctx, 400, "duplicate cache with name \"%s\"",name);
      return;
    }
  }
  if(!type || !strlen(type)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"type\" not found in <cache>");
    return;
  }
  if(!strcmp(type,"disk")) {
    cache = mapcache_cache_disk_create(ctx);
  } else if(!strcmp(type,"bdb")) {
#ifdef USE_BDB
    cache = mapcache_cache_bdb_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": Berkeley DB support is not available on this build",name);
    return;
#endif
  } else if(!strcmp(type,"tokyocabinet")) {
#ifdef USE_TC
    cache = mapcache_cache_tc_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": Tokyo Cabinet support is not available on this build",name);
    return;
#endif
  } else if(!strcmp(type,"sqlite3")) {
#ifdef USE_SQLITE
    cache = mapcache_cache_sqlite_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": sqlite support is not available on this build",name);
    return;
#endif
  } else if(!strcmp(type,"mbtiles")) {
#ifdef USE_SQLITE
    cache = mapcache_cache_mbtiles_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": sqlite support is not available on this build",name);
    return;
#endif
  } else if(!strcmp(type,"memcache")) {
#ifdef USE_MEMCACHE
    cache = mapcache_cache_memcache_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": memcache support is not available on this build",name);
    return;
#endif
  } else if(!strcmp(type,"tiff")) {
#ifdef USE_TIFF
    cache = mapcache_cache_tiff_create(ctx);
#else
    ctx->set_error(ctx,400, "failed to add cache \"%s\": tiff support is not available on this build",name);
    return;
#endif
  } else {
    ctx->set_error(ctx, 400, "unknown cache type %s for cache \"%s\"", type, name);
    return;
  }
  if(cache == NULL) {
    ctx->set_error(ctx, 400, "failed to parse cache \"%s\"", name);
    return;
  }
  cache->name = name;

  cache->configuration_parse_xml(ctx,node,cache,config);
  GC_CHECK_ERROR(ctx);
  mapcache_configuration_add_cache(config,cache,name);
  return;
}

void parseTileset(mapcache_context *ctx, ezxml_t node, mapcache_cfg *config)
{
  char *name = NULL;
  mapcache_tileset *tileset = NULL;
  ezxml_t cur_node;
  char* value;
  int havewgs84bbox=0;
  if(config->mode == MAPCACHE_MODE_NORMAL) {
    name = (char*)ezxml_attr(node,"name");
  } else {
    name = "mirror";
  }
  if(!name || !strlen(name)) {
    ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in <tileset>");
    return;
  } else {
    name = apr_pstrdup(ctx->pool, name);
    /* check we don't already have a cache defined with this name */
    if(mapcache_configuration_get_tileset(config, name)) {
      ctx->set_error(ctx, 400, "duplicate tileset with name \"%s\"",name);
      return;
    }
  }
  tileset = mapcache_tileset_create(ctx);
  tileset->name = name;

  if ((cur_node = ezxml_child(node,"read-only")) != NULL) {
    if(cur_node->txt && !strcmp(cur_node->txt,"true"))
      tileset->read_only = 1;
  }

  if ((cur_node = ezxml_child(node,"metadata")) != NULL) {
    parseMetadata(ctx, cur_node, tileset->metadata);
    GC_CHECK_ERROR(ctx);
  }


  if ((value = (char*)apr_table_get(tileset->metadata,"wgs84boundingbox")) != NULL) {
    double *values;
    int nvalues;
    value = apr_pstrdup(ctx->pool,value);
    if(MAPCACHE_SUCCESS != mapcache_util_extract_double_list(ctx, value, NULL, &values, &nvalues) ||
        nvalues != 4) {
      ctx->set_error(ctx, 400, "failed to parse extent array %s."
                     "(expecting 4 space separated numbers, got %d (%f %f %f %f)"
                     "eg <wgs84bbox>-180 -90 180 90</wgs84bbox>",
                     value,nvalues,values[0],values[1],values[2],values[3]);
      return;
    }
    tileset->wgs84bbox.minx = values[0];
    tileset->wgs84bbox.miny = values[1];
    tileset->wgs84bbox.maxx = values[2];
    tileset->wgs84bbox.maxy = values[3];
    havewgs84bbox = 1;
  }

  for(cur_node = ezxml_child(node,"grid"); cur_node; cur_node = cur_node->next) {
    mapcache_grid *grid;
    mapcache_grid_link *gridlink;
    char *restrictedExtent = NULL, *sTolerance = NULL;
    mapcache_extent *extent;
    int tolerance;

    if (tileset->grid_links == NULL) {
      tileset->grid_links = apr_array_make(ctx->pool,1,sizeof(mapcache_grid_link*));
    }
    grid = mapcache_configuration_get_grid(config, cur_node->txt);
    if(!grid) {
      ctx->set_error(ctx, 400, "tileset \"%s\" references grid \"%s\","
                     " but it is not configured", name, cur_node->txt);
      return;
    }
    gridlink = apr_pcalloc(ctx->pool,sizeof(mapcache_grid_link));
    gridlink->grid = grid;
    gridlink->minz = 0;
    gridlink->maxz = grid->nlevels;
    gridlink->grid_limits = (mapcache_extent_i*)apr_pcalloc(ctx->pool,grid->nlevels*sizeof(mapcache_extent_i));
    gridlink->outofzoom_strategy = MAPCACHE_OUTOFZOOM_NOTCONFIGURED;

    restrictedExtent = (char*)ezxml_attr(cur_node,"restricted_extent");
    if(restrictedExtent) {
      int nvalues;
      double *values;
      restrictedExtent = apr_pstrdup(ctx->pool,restrictedExtent);
      if(MAPCACHE_SUCCESS != mapcache_util_extract_double_list(ctx, restrictedExtent, NULL, &values, &nvalues) ||
          nvalues != 4) {
        ctx->set_error(ctx, 400, "failed to parse extent array %s."
                       "(expecting 4 space separated numbers, "
                       "eg <grid restricted_extent=\"-180 -90 180 90\">foo</grid>",
                       restrictedExtent);
        return;
      }
      gridlink->restricted_extent = (mapcache_extent*) apr_pcalloc(ctx->pool, sizeof(mapcache_extent));
      gridlink->restricted_extent->minx = values[0];
      gridlink->restricted_extent->miny = values[1];
      gridlink->restricted_extent->maxx = values[2];
      gridlink->restricted_extent->maxy = values[3];
      extent = gridlink->restricted_extent;
    } else {
      extent = &grid->extent;
    }

    tolerance = 5;
    sTolerance = (char*)ezxml_attr(cur_node,"tolerance");
    if(sTolerance) {
      char *endptr;
      tolerance = (int)strtol(sTolerance,&endptr,10);
      if(*endptr != 0 || tolerance < 0) {
        ctx->set_error(ctx, 400, "failed to parse grid tolerance %s (expecting a positive integer)",
                       sTolerance);
        return;
      }
    }


    mapcache_grid_compute_limits(grid,extent,gridlink->grid_limits,tolerance);

    sTolerance = (char*)ezxml_attr(cur_node,"minzoom");
    if(sTolerance) {
      char *endptr;
      tolerance = (int)strtol(sTolerance,&endptr,10);
      if(*endptr != 0 || tolerance < 0) {
        ctx->set_error(ctx, 400, "failed to parse grid minzoom %s (expecting a positive integer)",
                       sTolerance);
        return;
      }
      gridlink->minz = tolerance;
    }

    sTolerance = (char*)ezxml_attr(cur_node,"maxzoom");
    if(sTolerance) {
      char *endptr;
      tolerance = (int)strtol(sTolerance,&endptr,10);
      if(*endptr != 0 || tolerance < 0) {
        ctx->set_error(ctx, 400, "failed to parse grid maxzoom %s (expecting a positive integer)",
                       sTolerance);
        return;
      }
      gridlink->maxz = tolerance + 1;
    }

    if(gridlink->minz<0 || gridlink->maxz>grid->nlevels || gridlink->minz>=gridlink->maxz) {
      ctx->set_error(ctx, 400, "invalid grid maxzoom/minzoom %d/%d", gridlink->minz,gridlink->maxz);
      return;
    }

    sTolerance = (char*)ezxml_attr(cur_node,"max-cached-zoom");
    /* RFC97 implementation: check for a maximum zoomlevel to cache */
    if(sTolerance) {
      char *endptr;
      tolerance = (int)strtol(sTolerance,&endptr,10);
      if(*endptr != 0 || tolerance < 0) {
        ctx->set_error(ctx, 400, "failed to parse grid max-cached-zoom %s (expecting a positive integer)",
                       sTolerance);
        return;
      }

      if(tolerance > gridlink->maxz) {
        ctx->set_error(ctx, 400, "failed to parse grid max-cached-zoom %s (max cached zoom is greater than grid's max zoom)",
                       sTolerance);
        return;
      }
      gridlink->max_cached_zoom = tolerance;

      /* default to reassembling */
      gridlink->outofzoom_strategy = MAPCACHE_OUTOFZOOM_REASSEMBLE;
      sTolerance = (char*)ezxml_attr(cur_node,"out-of-zoom-strategy");
      if(sTolerance) {
        if(!strcasecmp(sTolerance,"reassemble")) {
          gridlink->outofzoom_strategy = MAPCACHE_OUTOFZOOM_REASSEMBLE;
        }
        else if(!strcasecmp(sTolerance,"proxy")) {
          gridlink->outofzoom_strategy = MAPCACHE_OUTOFZOOM_PROXY;
        } else {
          ctx->set_error(ctx, 400, "failed to parse grid out-of-zoom-strategy %s (expecting \"reassemble\" or \"proxy\")",
                        sTolerance);
          return;
        }
      }
    }



    /* compute wgs84 bbox if it wasn't supplied already */
    if(!havewgs84bbox && !strcasecmp(grid->srs,"EPSG:4326")) {
      tileset->wgs84bbox = *extent;
    }
    APR_ARRAY_PUSH(tileset->grid_links,mapcache_grid_link*) = gridlink;
  }

  if ((cur_node = ezxml_child(node,"dimensions")) != NULL) {
    parseDimensions(ctx, cur_node, tileset);
    GC_CHECK_ERROR(ctx);
  }

  if ((cur_node = ezxml_child(node,"timedimension")) != NULL) {
    parseTimeDimension(ctx, cur_node, tileset);
    GC_CHECK_ERROR(ctx);
  }


  if ((cur_node = ezxml_child(node,"cache")) != NULL) {
    mapcache_cache *cache = mapcache_configuration_get_cache(config, cur_node->txt);
    if(!cache) {
      ctx->set_error(ctx, 400, "tileset \"%s\" references cache \"%s\","
                     " but it is not configured", name, cur_node->txt);
      return;
    }
    tileset->cache = cache;
  }

  if ((cur_node = ezxml_child(node,"source")) != NULL) {
    mapcache_source *source = mapcache_configuration_get_source(config, cur_node->txt);
    if(!source) {
      ctx->set_error(ctx, 400, "tileset \"%s\" references source \"%s\","
                     " but it is not configured", name, cur_node->txt);
      return;
    }
    tileset->source = source;
  }

  if ((cur_node = ezxml_child(node,"metatile")) != NULL) {
    int *values, nvalues;
    value = apr_pstrdup(ctx->pool,cur_node->txt);

    if(MAPCACHE_SUCCESS != mapcache_util_extract_int_list(ctx, cur_node->txt, NULL,
        &values, &nvalues) ||
        nvalues != 2) {
      ctx->set_error(ctx, 400, "failed to parse metatile dimension %s."
                     "(expecting 2 space separated integers, "
                     "eg <metatile>5 5</metatile>",
                     cur_node->txt);
      return;
    }
    tileset->metasize_x = values[0];
    tileset->metasize_y = values[1];
  }


  if ((cur_node = ezxml_child(node,"watermark")) != NULL) {
    if(!*cur_node->txt) {
      ctx->set_error(ctx,400, "watermark config entry empty");
      return;
    }
    mapcache_tileset_add_watermark(ctx,tileset,cur_node->txt);
    GC_CHECK_ERROR(ctx);
  }


  if ((cur_node = ezxml_child(node,"expires")) != NULL) {
    char *endptr;
    tileset->expires = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx, 400, "failed to parse expires %s."
                     "(expecting an  integer, "
                     "eg <expires>3600</expires>",
                     cur_node->txt);
      return;
    }
  }
  if ((cur_node = ezxml_child(node,"auto_expire")) != NULL) {
    char *endptr;
    tileset->auto_expire = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx, 400, "failed to parse auto_expire %s."
                     "(expecting an  integer, "
                     "eg <auto_expire>3600</auto_expire>",
                     cur_node->txt);
      return;
    }
  }

  if ((cur_node = ezxml_child(node,"metabuffer")) != NULL) {
    char *endptr;
    tileset->metabuffer = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx, 400, "failed to parse metabuffer %s."
                     "(expecting an  integer, "
                     "eg <metabuffer>1</metabuffer>",
                     cur_node->txt);
      return;
    }
  }

  if ((cur_node = ezxml_child(node,"format")) != NULL) {
    mapcache_image_format *format = mapcache_configuration_get_image_format(config,cur_node->txt);
    if(!format) {
      ctx->set_error(ctx, 400, "tileset \"%s\" references format \"%s\","
                     " but it is not configured",name,cur_node->txt);
      return;
    }
    tileset->format = format;
  }

  mapcache_tileset_configuration_check(ctx,tileset);
  GC_CHECK_ERROR(ctx);
  mapcache_configuration_add_tileset(config,tileset,name);
  return;
}

void parseServices(mapcache_context *ctx, ezxml_t root, mapcache_cfg *config)
{
  ezxml_t node;
  if ((node = ezxml_child(root,"wms")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_WMS] = mapcache_service_wms_create(ctx);
    }
  }
  if ((node = ezxml_child(root,"wmts")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_WMTS] = mapcache_service_wmts_create(ctx);
    }
  }
  if ((node = ezxml_child(root,"ve")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_VE] = mapcache_service_ve_create(ctx);
    }
  }
  if ((node = ezxml_child(root,"tms")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_TMS] = mapcache_service_tms_create(ctx);
    }
  }
  if ((node = ezxml_child(root,"kml")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      if(!config->services[MAPCACHE_SERVICE_TMS]) {
        ctx->set_error(ctx,400,"kml service requires the tms service to be active");
        return;
      }
      config->services[MAPCACHE_SERVICE_KML] = mapcache_service_kml_create(ctx);
    }
  }

  if ((node = ezxml_child(root,"gmaps")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_GMAPS] = mapcache_service_gmaps_create(ctx);
    }
  }
  if ((node = ezxml_child(root,"demo")) != NULL) {
    if(!node->txt || !*node->txt || strcmp(node->txt, "false")) {
      config->services[MAPCACHE_SERVICE_DEMO] = mapcache_service_demo_create(ctx);
      if(!config->services[MAPCACHE_SERVICE_WMS])
        config->services[MAPCACHE_SERVICE_WMS] = mapcache_service_wms_create(ctx);
    }
  }

  if(!config->services[MAPCACHE_SERVICE_WMS] &&
      !config->services[MAPCACHE_SERVICE_TMS] &&
      !config->services[MAPCACHE_SERVICE_WMTS]) {
    ctx->set_error(ctx, 400, "no services configured."
                   " You must add a <services> tag with <wmts/> <wms/> or <tms/> children");
    return;
  }
}


void mapcache_configuration_parse_xml(mapcache_context *ctx, const char *filename, mapcache_cfg *config)
{
  ezxml_t doc, node;
  const char *mode;
  doc = ezxml_parse_file(filename);
  if (doc == NULL) {
    ctx->set_error(ctx,400, "failed to parse file %s. Is it valid XML?", filename);
    goto cleanup;
  } else {
    const char *err = ezxml_error(doc);
    if(err && *err) {
      ctx->set_error(ctx,400, "failed to parse file %s: %s", filename, err);
      goto cleanup;
    }
  }

  if(strcmp(doc->name,"mapcache")) {
    ctx->set_error(ctx,400, "failed to parse file %s. first node is not <mapcache>", filename);
    goto cleanup;
  }
  mode = ezxml_attr(doc,"mode");
  if(mode) {
    if(!strcmp(mode,"combined_mirror")) {
      config->mode = MAPCACHE_MODE_MIRROR_COMBINED;
    } else if(!strcmp(mode,"split_mirror")) {
      config->mode = MAPCACHE_MODE_MIRROR_SPLIT;
    } else if(!strcmp(mode,"normal")) {
      config->mode = MAPCACHE_MODE_NORMAL;
    } else {
      ctx->set_error(ctx,400,"unknown mode \"%s\" for <mapcache>",mode);
      goto cleanup;
    }
  } else {
    config->mode = MAPCACHE_MODE_NORMAL;
  }

  for(node = ezxml_child(doc,"metadata"); node; node = node->next) {
    parseMetadata(ctx, node, config->metadata);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  for(node = ezxml_child(doc,"source"); node; node = node->next) {
    parseSource(ctx, node, config);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  for(node = ezxml_child(doc,"grid"); node; node = node->next) {
    parseGrid(ctx, node, config);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  for(node = ezxml_child(doc,"format"); node; node = node->next) {
    parseFormat(ctx, node, config);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  for(node = ezxml_child(doc,"cache"); node; node = node->next) {
    parseCache(ctx, node, config);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  for(node = ezxml_child(doc,"tileset"); node; node = node->next) {
    parseTileset(ctx, node, config);
    if(GC_HAS_ERROR(ctx)) goto cleanup;
  }

  if ((node = ezxml_child(doc,"service")) != NULL) {
    ezxml_t service_node;
    for(service_node = node; service_node; service_node = service_node->next) {
      char *enabled = (char*)ezxml_attr(service_node,"enabled");
      char *type = (char*)ezxml_attr(service_node,"type");
      if(!strcasecmp(enabled,"true")) {
        if (!strcasecmp(type,"wms")) {
          mapcache_service *new_service = mapcache_service_wms_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_WMS] = new_service;
        } else if (!strcasecmp(type,"tms")) {
          mapcache_service *new_service = mapcache_service_tms_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_TMS] = new_service;
        } else if (!strcasecmp(type,"wmts")) {
          mapcache_service *new_service = mapcache_service_wmts_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_WMTS] = new_service;
        } else if (!strcasecmp(type,"kml")) {
          mapcache_service *new_service = mapcache_service_kml_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_KML] = new_service;
        } else if (!strcasecmp(type,"gmaps")) {
          mapcache_service *new_service = mapcache_service_gmaps_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_GMAPS] = new_service;
        } else if (!strcasecmp(type,"mapguide")) {
          mapcache_service *new_service = mapcache_service_mapguide_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_MAPGUIDE] = new_service;
        } else if (!strcasecmp(type,"ve")) {
          mapcache_service *new_service = mapcache_service_ve_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_VE] = new_service;
        } else if (!strcasecmp(type,"demo")) {
          mapcache_service *new_service = mapcache_service_demo_create(ctx);
          if(new_service->configuration_parse_xml) {
            new_service->configuration_parse_xml(ctx,service_node,new_service,config);
          }
          config->services[MAPCACHE_SERVICE_DEMO] = new_service;
        } else {
          ctx->set_error(ctx,400,"unknown <service> type %s",type);
        }
        if(GC_HAS_ERROR(ctx)) goto cleanup;
      }
    }
  } else if ((node = ezxml_child(doc,"services")) != NULL) {
    ctx->log(ctx,MAPCACHE_WARN,"<services> tag is deprecated, use <service type=\"wms\" enabled=\"true|false\">");
    parseServices(ctx, node, config);
  } else {
    ctx->set_error(ctx, 400, "no <services> configured");
  }
  if(GC_HAS_ERROR(ctx)) goto cleanup;


  node = ezxml_child(doc,"default_format");
  if(!node)
    node = ezxml_child(doc,"merge_format");
  if (node) {
    mapcache_image_format *format = mapcache_configuration_get_image_format(config,node->txt);
    if(!format) {
      ctx->set_error(ctx, 400, "default_format tag references format %s but it is not configured",
                     node->txt);
      goto cleanup;
    }
    config->default_image_format = format;
  }

  if ((node = ezxml_child(doc,"errors")) != NULL) {
    if(!strcmp(node->txt,"log")) {
      config->reporting = MAPCACHE_REPORT_LOG;
    } else if(!strcmp(node->txt,"report")) {
      config->reporting = MAPCACHE_REPORT_MSG;
    } else if(!strcmp(node->txt,"empty_img")) {
      config->reporting = MAPCACHE_REPORT_EMPTY_IMG;
      mapcache_image_create_empty(ctx, config);
      if(GC_HAS_ERROR(ctx)) goto cleanup;
    } else if(!strcmp(node->txt, "report_img")) {
      config->reporting = MAPCACHE_REPORT_ERROR_IMG;
      ctx->set_error(ctx,501,"<errors>: report_img not implemented");
      goto cleanup;
    } else {
      ctx->set_error(ctx,400,"<errors>: unknown value %s (allowed are log, report, empty_img, report_img)",
                     node->txt);
      goto cleanup;
    }
  }

  if((node = ezxml_child(doc,"lock_dir")) != NULL) {
    config->lockdir = apr_pstrdup(ctx->pool, node->txt);
  } else {
    config->lockdir = apr_pstrdup(ctx->pool,"/tmp");
  }

  if((node = ezxml_child(doc,"lock_retry")) != NULL) {
    char *endptr;
    config->lock_retry_interval = (unsigned int)strtol(node->txt,&endptr,10);
    if(*endptr != 0 || config->lock_retry_interval < 0) {
      ctx->set_error(ctx, 400, "failed to parse lock_retry microseconds \"%s\". Expecting a positive integer",
                     node->txt);
      return;
    }
  }

  if((node = ezxml_child(doc,"threaded_fetching")) != NULL) {
    if(!strcasecmp(node->txt,"true")) {
      config->threaded_fetching = 1;
    } else if(strcasecmp(node->txt,"false")) {
      ctx->set_error(ctx, 400, "failed to parse threaded_fetching \"%s\". Expecting true or false",node->txt);
      return;
    }
  }

  if((node = ezxml_child(doc,"log_level")) != NULL) {
    if(!strcasecmp(node->txt,"debug")) {
      config->loglevel = MAPCACHE_DEBUG;
    } else if(!strcasecmp(node->txt,"info")) {
      config->loglevel = MAPCACHE_INFO;
    } else if(!strcasecmp(node->txt,"notice")) {
      config->loglevel = MAPCACHE_NOTICE;
    } else if(!strcasecmp(node->txt,"warn")) {
      config->loglevel = MAPCACHE_WARN;
    } else if(!strcasecmp(node->txt,"error")) {
      config->loglevel = MAPCACHE_ERROR;
    } else if(!strcasecmp(node->txt,"crit")) {
      config->loglevel = MAPCACHE_CRIT;
    } else if(!strcasecmp(node->txt,"alert")) {
      config->loglevel = MAPCACHE_ALERT;
    } else if(!strcasecmp(node->txt,"emerg")) {
      config->loglevel = MAPCACHE_EMERG;
    } else {
      ctx->set_error(ctx,500,"failed to parse <log_level> \"%s\". Expecting debug, info, notice, warn, error, crit, alert or emerg",node->txt);
      return;
    }
  }
  if((node = ezxml_child(doc,"auto_reload")) != NULL) {
    if(!strcasecmp(node->txt,"true")) {
      config->autoreload = 1;
    } else if(!strcasecmp(node->txt,"false")) {
      config->autoreload = 0;
    } else {
      ctx->set_error(ctx,500,"failed to parse <auto_reload> \"%s\". Expecting true or false",node->txt);
      return;
    }
  }


cleanup:
  ezxml_free(doc);
  return;
}
/* vim: ts=2 sts=2 et sw=2
*/
