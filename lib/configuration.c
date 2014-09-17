/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: high level configuration file parser
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
#include <apr_file_info.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <math.h>

void mapcache_configuration_parse(mapcache_context *ctx, const char *filename, mapcache_cfg *config, int cgi)
{
  apr_dir_t *lockdir;
  apr_status_t rv;
  char errmsg[120];
  char *url;

  mapcache_configuration_parse_xml(ctx,filename,config);


  GC_CHECK_ERROR(ctx);

  if(!config->lockdir || !strlen(config->lockdir)) {
    config->lockdir = apr_pstrdup(ctx->pool, "/tmp");
  }
  rv = apr_dir_open(&lockdir,config->lockdir,ctx->pool);
  if(rv != APR_SUCCESS) {
    ctx->set_error(ctx,500, "failed to open lock directory %s: %s"
                   ,config->lockdir,apr_strerror(rv,errmsg,120));
    return;
  }

  /* only remove lockfiles if we're not in cgi mode */
  if(!cgi) {
    apr_finfo_t finfo;
    while ((apr_dir_read(&finfo, APR_FINFO_DIRENT|APR_FINFO_TYPE|APR_FINFO_NAME, lockdir)) == APR_SUCCESS) {
      if(finfo.filetype == APR_REG) {
        if(!strncmp(finfo.name, MAPCACHE_LOCKFILE_PREFIX, strlen(MAPCACHE_LOCKFILE_PREFIX))) {
          ctx->log(ctx,MAPCACHE_WARN,"found old lockfile %s/%s, deleting it",config->lockdir,
                   finfo.name);
          rv = apr_file_remove(apr_psprintf(ctx->pool,"%s/%s",config->lockdir, finfo.name),ctx->pool);
          if(rv != APR_SUCCESS) {
            ctx->set_error(ctx,500, "failed to remove lockfile %s: %s",finfo.name,apr_strerror(rv,errmsg,120));
            return;
          }

        }

      }
    }
  }
  apr_dir_close(lockdir);

  /* if we were suppplied with an onlineresource, make sure it ends with a / */
  if(NULL != (url = (char*)apr_table_get(config->metadata,"url"))) {
    char *urlend = url + strlen(url)-1;
    if(*urlend != '/') {
      url = apr_pstrcat(ctx->pool,url,"/",NULL);
      apr_table_setn(config->metadata,"url",url);
    }
  }
}

void mapcache_configuration_post_config(mapcache_context *ctx, mapcache_cfg *config)
{
  apr_hash_index_t *cachei = apr_hash_first(ctx->pool,config->caches);
  while(cachei) {
    mapcache_cache *cache;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(cachei,&key,&keylen,(void**)&cache);
    cache->configuration_post_config(ctx,cache,config);
    GC_CHECK_ERROR(ctx);
    cachei = apr_hash_next(cachei);
  }
}


mapcache_cfg* mapcache_configuration_create(apr_pool_t *pool)
{
  mapcache_grid *grid;
  int i;
  double wgs84_resolutions[18]= {
    0.703125000000000,
    0.351562500000000,
    0.175781250000000,
    8.78906250000000e-2,
    4.39453125000000e-2,
    2.19726562500000e-2,
    1.09863281250000e-2,
    5.49316406250000e-3,
    2.74658203125000e-3,
    1.37329101562500e-3,
    6.86645507812500e-4,
    3.43322753906250e-4,
    1.71661376953125e-4,
    8.58306884765625e-5,
    4.29153442382812e-5,
    2.14576721191406e-5,
    1.07288360595703e-5,
    5.36441802978516e-6
  };

  double google_resolutions[19] = {
    156543.0339280410,
    78271.51696402048,
    39135.75848201023,
    19567.87924100512,
    9783.939620502561,
    4891.969810251280,
    2445.984905125640,
    1222.992452562820,
    611.4962262814100,
    305.7481131407048,
    152.8740565703525,
    76.43702828517624,
    38.21851414258813,
    19.10925707129406,
    9.554628535647032,
    4.777314267823516,
    2.388657133911758,
    1.194328566955879,
    0.5971642834779395
  };




  mapcache_extent wgs84_extent= {-180,-90,180,90};
  mapcache_extent google_extent= {-20037508.3427892480,-20037508.3427892480,20037508.3427892480,20037508.3427892480};
  double unitwidth,unitheight;

  mapcache_cfg *cfg = (mapcache_cfg*)apr_pcalloc(pool, sizeof(mapcache_cfg));
  cfg->caches = apr_hash_make(pool);
  cfg->sources = apr_hash_make(pool);
  cfg->tilesets = apr_hash_make(pool);
  cfg->grids = apr_hash_make(pool);
  cfg->image_formats = apr_hash_make(pool);
  cfg->metadata = apr_table_make(pool,3);

  mapcache_configuration_add_image_format(cfg,
          mapcache_imageio_create_png_format(pool,"PNG",MAPCACHE_COMPRESSION_FAST),
          "PNG");
  mapcache_configuration_add_image_format(cfg,
          mapcache_imageio_create_png_q_format(pool,"PNG8",MAPCACHE_COMPRESSION_FAST,256),
          "PNG8");
  mapcache_configuration_add_image_format(cfg,
          mapcache_imageio_create_gif_format(pool,"GIF"),
          "GIF");
  mapcache_configuration_add_image_format(cfg,
          mapcache_imageio_create_jpeg_format(pool,"JPEG",90,MAPCACHE_PHOTOMETRIC_YCBCR),
          "JPEG");
  mapcache_configuration_add_image_format(cfg,
          mapcache_imageio_create_mixed_format(pool,"mixed",
                    mapcache_configuration_get_image_format(cfg,"PNG"),
                    mapcache_configuration_get_image_format(cfg,"JPEG")),
          "mixed");
  cfg->default_image_format = mapcache_configuration_get_image_format(cfg,"mixed");
  cfg->reporting = MAPCACHE_REPORT_MSG;

  grid = mapcache_grid_create(pool);
  grid->name = apr_pstrdup(pool,"WGS84");
  apr_table_add(grid->metadata,"title","GoogleCRS84Quad");
  apr_table_add(grid->metadata,"wellKnownScaleSet","urn:ogc:def:wkss:OGC:1.0:GoogleCRS84Quad");
  apr_table_add(grid->metadata,"profile","global-geodetic");
  grid->srs = apr_pstrdup(pool,"EPSG:4326");
  grid->unit = MAPCACHE_UNIT_DEGREES;
  grid->tile_sx = grid->tile_sy = 256;
  grid->nlevels = 18;
  grid->extent = wgs84_extent;
  grid->levels = (mapcache_grid_level**)apr_pcalloc(pool,
                 grid->nlevels*sizeof(mapcache_grid_level*));
  for(i=0; i<grid->nlevels; i++) {
    mapcache_grid_level *level = (mapcache_grid_level*)apr_pcalloc(pool,sizeof(mapcache_grid_level));
    level->resolution = wgs84_resolutions[i];
    unitheight = grid->tile_sy * level->resolution;
    unitwidth = grid->tile_sx * level->resolution;

    level->maxy = ceil((grid->extent.maxy-grid->extent.miny - 0.01* unitheight)/unitheight);
    level->maxx = ceil((grid->extent.maxx-grid->extent.minx - 0.01* unitwidth)/unitwidth);
    grid->levels[i] = level;
  }
  mapcache_configuration_add_grid(cfg,grid,"WGS84");

  grid = mapcache_grid_create(pool);
  grid->name = apr_pstrdup(pool,"GoogleMapsCompatible");
  grid->srs = apr_pstrdup(pool,"EPSG:3857");
  APR_ARRAY_PUSH(grid->srs_aliases,char*) = apr_pstrdup(pool,"EPSG:900913");
  apr_table_add(grid->metadata,"title","GoogleMapsCompatible");
  apr_table_add(grid->metadata,"profile","global-mercator");
  apr_table_add(grid->metadata,"wellKnownScaleSet","urn:ogc:def:wkss:OGC:1.0:GoogleMapsCompatible");
  grid->tile_sx = grid->tile_sy = 256;
  grid->nlevels = 19;
  grid->unit = MAPCACHE_UNIT_METERS;
  grid->extent = google_extent;
  grid->levels = (mapcache_grid_level**)apr_pcalloc(pool,
                 grid->nlevels*sizeof(mapcache_grid_level*));
  for(i=0; i<grid->nlevels; i++) {
    mapcache_grid_level *level = (mapcache_grid_level*)apr_pcalloc(pool,sizeof(mapcache_grid_level));
    level->resolution = google_resolutions[i];
    unitheight = grid->tile_sy * level->resolution;
    unitwidth = grid->tile_sx * level->resolution;

    level->maxy = ceil((grid->extent.maxy-grid->extent.miny - 0.01* unitheight)/unitheight);
    level->maxx = ceil((grid->extent.maxx-grid->extent.minx - 0.01* unitwidth)/unitwidth);
    grid->levels[i] = level;
  }
  mapcache_configuration_add_grid(cfg,grid,"GoogleMapsCompatible");

  grid = mapcache_grid_create(pool);
  grid->name = apr_pstrdup(pool,"g");
  grid->srs = apr_pstrdup(pool,"EPSG:900913");
  APR_ARRAY_PUSH(grid->srs_aliases,char*) = apr_pstrdup(pool,"EPSG:3857");
  apr_table_add(grid->metadata,"title","GoogleMapsCompatible");
  apr_table_add(grid->metadata,"profile","global-mercator");
  apr_table_add(grid->metadata,"wellKnownScaleSet","urn:ogc:def:wkss:OGC:1.0:GoogleMapsCompatible");
  grid->tile_sx = grid->tile_sy = 256;
  grid->nlevels = 19;
  grid->unit = MAPCACHE_UNIT_METERS;
  grid->extent = google_extent;
  grid->levels = (mapcache_grid_level**)apr_pcalloc(pool,
                 grid->nlevels*sizeof(mapcache_grid_level*));
  for(i=0; i<grid->nlevels; i++) {
    mapcache_grid_level *level = (mapcache_grid_level*)apr_pcalloc(pool,sizeof(mapcache_grid_level));
    level->resolution = google_resolutions[i];
    unitheight = grid->tile_sy * level->resolution;
    unitwidth = grid->tile_sx * level->resolution;

    level->maxy = ceil((grid->extent.maxy-grid->extent.miny - 0.01* unitheight)/unitheight);
    level->maxx = ceil((grid->extent.maxx-grid->extent.minx - 0.01* unitwidth)/unitwidth);
    grid->levels[i] = level;
  }
  mapcache_configuration_add_grid(cfg,grid,"g");

  /* default retry interval is 1/100th of a second, i.e. 10000 microseconds */
  cfg->lock_retry_interval = 10000;

  cfg->loglevel = MAPCACHE_WARN;
  cfg->autoreload = 0;

  return cfg;
}

mapcache_source *mapcache_configuration_get_source(mapcache_cfg *config, const char *key)
{
  return (mapcache_source*)apr_hash_get(config->sources, (void*)key, APR_HASH_KEY_STRING);
}

mapcache_cache *mapcache_configuration_get_cache(mapcache_cfg *config, const char *key)
{
  return (mapcache_cache*)apr_hash_get(config->caches, (void*)key, APR_HASH_KEY_STRING);
}

mapcache_grid *mapcache_configuration_get_grid(mapcache_cfg *config, const char *key)
{
  return (mapcache_grid*)apr_hash_get(config->grids, (void*)key, APR_HASH_KEY_STRING);
}

mapcache_tileset *mapcache_configuration_get_tileset(mapcache_cfg *config, const char *key)
{
  if(config->mode == MAPCACHE_MODE_NORMAL) {
    return (mapcache_tileset*)apr_hash_get(config->tilesets, (void*)key, APR_HASH_KEY_STRING);
  } else {
    return (mapcache_tileset*)apr_hash_get(config->tilesets, (void*)"mirror", APR_HASH_KEY_STRING);
  }
}

mapcache_image_format *mapcache_configuration_get_image_format(mapcache_cfg *config, const char *key)
{
  return (mapcache_image_format*)apr_hash_get(config->image_formats, (void*)key, APR_HASH_KEY_STRING);
}

void mapcache_configuration_add_source(mapcache_cfg *config, mapcache_source *source, const char * key)
{
  apr_hash_set(config->sources, key, APR_HASH_KEY_STRING, (void*)source);
}

void mapcache_configuration_add_grid(mapcache_cfg *config, mapcache_grid *grid, const char * key)
{
  apr_hash_set(config->grids, key, APR_HASH_KEY_STRING, (void*)grid);
}

void mapcache_configuration_add_tileset(mapcache_cfg *config, mapcache_tileset *tileset, const char * key)
{
  tileset->config = config;
  apr_hash_set(config->tilesets, key, APR_HASH_KEY_STRING, (void*)tileset);
}

void mapcache_configuration_add_cache(mapcache_cfg *config, mapcache_cache *cache, const char * key)
{
  apr_hash_set(config->caches, key, APR_HASH_KEY_STRING, (void*)cache);
}

void mapcache_configuration_add_image_format(mapcache_cfg *config, mapcache_image_format *format, const char * key)
{
  apr_hash_set(config->image_formats, key, APR_HASH_KEY_STRING, (void*)format);
}
