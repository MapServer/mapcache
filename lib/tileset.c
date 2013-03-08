/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: high level tile access
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
#include <apr_strings.h>
#include <apr_file_info.h>
#include <apr_file_io.h>
#include <math.h>

#ifdef _WIN32
#include <limits.h>
#endif

char* mapcache_tileset_metatile_resource_key(mapcache_context *ctx, mapcache_metatile *mt)
{
  char *lockname = apr_psprintf(ctx->pool,
                                "%d-%d-%d-%s",
                                mt->z,mt->y,mt->x,
                                mt->map.tileset->name);

  /* if the tileset has multiple grids, add the name of the current grid to the lock key*/
  if(mt->map.tileset->grid_links->nelts > 1) {
    lockname = apr_pstrcat(ctx->pool,lockname,mt->map.grid_link->grid->name,NULL);
  }

  if(mt->map.dimensions && !apr_is_empty_table(mt->map.dimensions)) {
    const apr_array_header_t *elts = apr_table_elts(mt->map.dimensions);
    int i;
    for(i=0; i<elts->nelts; i++) {
      apr_table_entry_t entry = APR_ARRAY_IDX(elts,i,apr_table_entry_t);
      char *dimvalue = apr_pstrdup(ctx->pool,entry.val);
      char *iter = dimvalue;
      while(*iter) {
        if(*iter == '/') *iter='_';
        iter++;
      }
      lockname = apr_pstrcat(ctx->pool,lockname,dimvalue,NULL);
    }

  }
  return lockname;
}

void mapcache_tileset_configuration_check(mapcache_context *ctx, mapcache_tileset *tileset)
{

  /* check we have all we want */
  if(tileset->cache == NULL) {
    /* TODO: we should allow tilesets with no caches */
    ctx->set_error(ctx, 400, "tileset \"%s\" has no cache configured.", tileset->name);
    return;
  }

  if(apr_is_empty_array(tileset->grid_links)) {
    ctx->set_error(ctx, 400, "tileset \"%s\" has no grids configured", tileset->name);
    return;
  }
#ifdef USE_PROJ
  /* not implemented yet, will be used to automatically calculate wgs84bbox with proj */
  else if(tileset->wgs84bbox[0]>=tileset->wgs84bbox[2]) {
    mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
    double *extent = sgrid->grid->extent;
    if(sgrid->restricted_extent) {
      extent = sgrid->restricted_extent;
    }
  }
#endif
  if(!tileset->format && tileset->source && tileset->source->type == MAPCACHE_SOURCE_GDAL) {
    ctx->set_error(ctx,400, "tileset \"%s\" references a gdal source. <format> tag is missing and mandatory in this case",
                   tileset->name);
    return;
  }

  if(tileset->metabuffer < 0 || tileset->metasize_x < 1 || tileset->metasize_y < 1) {
    ctx->set_error(ctx,400,"tileset \"%s\" has invalid metasize %d,%d or metabuffer %d",
                   tileset->name,tileset->metasize_x,tileset->metasize_y,tileset->metabuffer);
    return;
  }

  if(!tileset->format && (
        tileset->metasize_x != 1 ||
        tileset->metasize_y != 1 ||
        tileset->metabuffer != 0 ||
        tileset->watermark)) {
    if(tileset->watermark) {
      ctx->set_error(ctx,400,"tileset \"%s\" has no <format> configured, but it is needed for the watermark",
                     tileset->name);
      return;
    } else {
      ctx->set_error(ctx,400,"tileset \"%s\" has no <format> configured, but it is needed for metatiling",
                     tileset->name);
      return;
    }
  }
}

void mapcache_tileset_add_watermark(mapcache_context *ctx, mapcache_tileset *tileset, const char *filename)
{
  apr_file_t *f;
  apr_finfo_t finfo;
  int rv;
  mapcache_buffer *watermarkdata;
  apr_size_t size;
  if(apr_file_open(&f, filename, APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,
                   APR_OS_DEFAULT, ctx->pool) != APR_SUCCESS) {
    ctx->set_error(ctx,500, "failed to open watermark image %s",filename);
    return;
  }
  rv = apr_file_info_get(&finfo, APR_FINFO_SIZE, f);
  if(rv != APR_SUCCESS || !finfo.size) {
    ctx->set_error(ctx, 500, "watermark %s has no data",filename);
    return;
  }

  watermarkdata = mapcache_buffer_create(finfo.size,ctx->pool);
  //manually add the data to our buffer
  size = finfo.size;
  apr_file_read(f,watermarkdata->buf,&size);
  watermarkdata->size = size;
  if(size != finfo.size) {
    ctx->set_error(ctx, 500,  "failed to copy watermark image data, got %d of %d bytes",(int)size, (int)finfo.size);
    return;
  }
  apr_file_close(f);
  tileset->watermark = mapcache_imageio_decode(ctx,watermarkdata);
}

void mapcache_tileset_tile_validate(mapcache_context *ctx, mapcache_tile *tile)
{
  mapcache_extent_i limits;
  if(tile->z < tile->grid_link->minz || tile->z >= tile->grid_link->maxz) {
    ctx->set_error(ctx,404,"invalid tile z level");
    return;
  }
  limits = tile->grid_link->grid_limits[tile->z];
  if(tile->x<limits.minx || tile->x>=limits.maxx) {
    ctx->set_error(ctx, 404, "tile x=%d not in [%d,%d[",
                   tile->x,limits.minx,limits.maxx);
    return;
  }
  if(tile->y<limits.miny || tile->y>=limits.maxy) {
    ctx->set_error(ctx, 404, "tile y=%d not in [%d,%d[",
                   tile->y,limits.miny,limits.maxy);
    return;
  }
}


void mapcache_tileset_get_map_tiles(mapcache_context *ctx, mapcache_tileset *tileset,
                                    mapcache_grid_link *grid_link,
                                    mapcache_extent *bbox, int width, int height,
                                    int *ntiles,
                                    mapcache_tile ***tiles)
{
  double resolution;
  int level;
  int bl_x,bl_y,tr_x,tr_y;
  int mx,my,Mx,My;
  int x,y;
  int i=0;
  resolution = mapcache_grid_get_resolution(bbox, width, height);
  mapcache_grid_get_closest_level(ctx,grid_link,resolution,&level);

  mapcache_grid_get_xy(ctx,grid_link->grid,bbox->minx,bbox->miny,level,&bl_x,&bl_y);
  mapcache_grid_get_xy(ctx,grid_link->grid,bbox->maxx,bbox->maxy,level,&tr_x,&tr_y);
  Mx = MAPCACHE_MAX(tr_x,bl_x);
  My = MAPCACHE_MAX(tr_y,bl_y);
  mx = MAPCACHE_MIN(tr_x,bl_x);
  my = MAPCACHE_MIN(tr_y,bl_y);
  *ntiles = (Mx-mx+1)*(My-my+1);
  i=0;
  *tiles = (mapcache_tile**)apr_pcalloc(ctx->pool, *ntiles*sizeof(mapcache_tile*));
  for(x=mx; x<=Mx; x++) {
    for(y=my; y<=My; y++) {
      mapcache_tile *tile = mapcache_tileset_tile_create(ctx->pool,tileset, grid_link);
      tile->x = x;
      tile->y = y;
      tile->z = level;
      mapcache_tileset_tile_validate(ctx,tile);
      if(GC_HAS_ERROR(ctx)) {
        //clear the error message
        ctx->clear_errors(ctx);
      } else {
        (*tiles)[i++]=tile;
      }
    }
  }
  *ntiles = i;
}

mapcache_image* mapcache_tileset_assemble_map_tiles(mapcache_context *ctx, mapcache_tileset *tileset,
    mapcache_grid_link *grid_link,
    mapcache_extent *bbox, int width, int height,
    int ntiles,
    mapcache_tile **tiles,
    mapcache_resample_mode mode)
{
  double hresolution = mapcache_grid_get_horizontal_resolution(bbox, width);
  double vresolution = mapcache_grid_get_vertical_resolution(bbox, height);
  mapcache_extent tilebbox;
  mapcache_tile *toplefttile=NULL;
  int mx=INT_MAX,my=INT_MAX,Mx=INT_MIN,My=INT_MIN;
  int i;
  mapcache_image *image = mapcache_image_create(ctx);
  mapcache_image *srcimage;
  double tileresolution, dstminx, dstminy, hf, vf;
#ifdef DEBUG
  /* we know at least one tile contains data */
  for(i=0; i<ntiles; i++) {
    if(!tiles[i]->nodata) {
      break;
    }
  }
  if(i==ntiles) {
    ctx->set_error(ctx,500,"###BUG#### mapcache_tileset_assemble_map_tiles called with no tiles containing data");
    return NULL;
  }
#endif

  image->w = width;
  image->h = height;
  image->stride = width*4;
  image->data = calloc(1,width*height*4*sizeof(unsigned char));
  apr_pool_cleanup_register(ctx->pool, image->data, (void*)free, apr_pool_cleanup_null) ;
  if(ntiles == 0) {
    return image;
  }

  /* compute the number of tiles horizontally and vertically */
  for(i=0; i<ntiles; i++) {
    mapcache_tile *tile = tiles[i];
    if(tile->x < mx) mx = tile->x;
    if(tile->y < my) my = tile->y;
    if(tile->x > Mx) Mx = tile->x;
    if(tile->y > My) My = tile->y;
  }
  /* create image that will contain the unscaled tiles data */
  srcimage = mapcache_image_create(ctx);
  srcimage->w = (Mx-mx+1)*tiles[0]->grid_link->grid->tile_sx;
  srcimage->h = (My-my+1)*tiles[0]->grid_link->grid->tile_sy;
  srcimage->stride = srcimage->w*4;
  srcimage->data = calloc(1,srcimage->w*srcimage->h*4*sizeof(unsigned char));
  apr_pool_cleanup_register(ctx->pool, srcimage->data, (void*)free, apr_pool_cleanup_null) ;

  /* copy the tiles data into the src image */
  for(i=0; i<ntiles; i++) {
    int ox,oy; /* the offset from the start of the src image to the start of the tile */
    mapcache_image fakeimg;
    mapcache_tile *tile = tiles[i];
    switch(grid_link->grid->origin) {
      case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
        if(tile->x == mx && tile->y == My) {
          toplefttile = tile;
        }
        ox = (tile->x - mx) * tile->grid_link->grid->tile_sx;
        oy = (My - tile->y) * tile->grid_link->grid->tile_sy;
        break;
      case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
        if(tile->x == mx && tile->y == my) {
          toplefttile = tile;
        }
        ox = (tile->x - mx) * tile->grid_link->grid->tile_sx;
        oy = (tile->y - my) * tile->grid_link->grid->tile_sy;
        break;
      case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
        if(tile->x == Mx && tile->y == My) {
          toplefttile = tile;
        }
        ox = (Mx - tile->x) * tile->grid_link->grid->tile_sx;
        oy = (My - tile->y) * tile->grid_link->grid->tile_sy;
        break;
      case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
        if(tile->x == Mx && tile->y == my) {
          toplefttile = tile;
        }
        ox = (Mx - tile->x) * tile->grid_link->grid->tile_sx;
        oy = (tile->y - my) * tile->grid_link->grid->tile_sy;
        break;
    }
    if(tile->nodata) continue;


    fakeimg.stride = srcimage->stride;
    fakeimg.data = &(srcimage->data[oy*srcimage->stride+ox*4]);
    if(!tile->raw_image) {
      mapcache_imageio_decode_to_image(ctx,tile->encoded_data,&fakeimg);
    } else {
      int r;
      unsigned char *srcptr = tile->raw_image->data;
      unsigned char *dstptr = fakeimg.data;
      for(r=0; r<tile->raw_image->h; r++) {
        memcpy(dstptr,srcptr,tile->raw_image->stride);
        srcptr += tile->raw_image->stride;
        dstptr += fakeimg.stride;
      }
    }
  }

  assert(toplefttile);

  /* copy/scale the srcimage onto the destination image */
  tileresolution = toplefttile->grid_link->grid->levels[toplefttile->z]->resolution;
  mapcache_grid_get_extent(ctx,toplefttile->grid_link->grid,
                           toplefttile->x, toplefttile->y, toplefttile->z, &tilebbox);

  /*compute the pixel position of top left corner*/
  dstminx = (tilebbox.minx-bbox->minx)/hresolution;
  dstminy = (bbox->maxy-tilebbox.maxy)/vresolution;
  hf = tileresolution/hresolution;
  vf = tileresolution/vresolution;
  if(fabs(hf-1)<0.0001 && fabs(vf-1)<0.0001) {
    //use nearest resampling if we are at the resolution of the tiles
    mapcache_image_copy_resampled_nearest(ctx,srcimage,image,dstminx,dstminy,hf,vf);
  } else {
    switch(mode) {
      case MAPCACHE_RESAMPLE_BILINEAR:
        mapcache_image_copy_resampled_bilinear(ctx,srcimage,image,dstminx,dstminy,hf,vf);
        break;
      default:
        mapcache_image_copy_resampled_nearest(ctx,srcimage,image,dstminx,dstminy,hf,vf);
        break;
    }
  }
  return image;
}

/*
 * compute the metatile that should be rendered for the given tile
 */
mapcache_metatile* mapcache_tileset_metatile_get(mapcache_context *ctx, mapcache_tile *tile)
{
  mapcache_metatile *mt = (mapcache_metatile*)apr_pcalloc(ctx->pool, sizeof(mapcache_metatile));
  int i,j,blx,bly;
  mapcache_tileset *tileset = tile->tileset;
  mapcache_grid *grid = tile->grid_link->grid;
  double res = grid->levels[tile->z]->resolution;
  double gbuffer,gwidth,gheight,fullgwidth,fullgheight;
  mt->map.tileset = tileset;
  mt->map.grid_link = tile->grid_link;
  mt->z = tile->z;
  mt->x = tile->x / tileset->metasize_x;
  if(tile->x < 0)
    mt->x --;
  mt->y = tile->y / tileset->metasize_y;
  if(tile->y < 0)
    mt->y --;
  blx = mt->x * tileset->metasize_x;
  bly = mt->y * tileset->metasize_y;

  /* adjust the size of the the metatile so it does not extend past the grid limits.
   * If we don't do this, we end up with cut labels on the edges of the tile grid
   */
  if(blx+tileset->metasize_x-1 >= grid->levels[tile->z]->maxx) {
    mt->metasize_x = grid->levels[tile->z]->maxx - blx;
  } else {
    mt->metasize_x = tileset->metasize_x;
  }
  if(bly+tileset->metasize_y-1 >= grid->levels[tile->z]->maxy) {
    mt->metasize_y = grid->levels[tile->z]->maxy - bly;
  } else {
    mt->metasize_y = tileset->metasize_y;
  }

  mt->ntiles = mt->metasize_x * mt->metasize_y;
  mt->tiles = (mapcache_tile*)apr_pcalloc(ctx->pool, mt->ntiles * sizeof(mapcache_tile));
  mt->map.width =  mt->metasize_x * grid->tile_sx + 2 * tileset->metabuffer;
  mt->map.height =  mt->metasize_y * grid->tile_sy + 2 * tileset->metabuffer;
  mt->map.dimensions = tile->dimensions;

  /* buffer in geographical units */
  gbuffer = res * tileset->metabuffer;

  /* adjusted metatile size in geographical units */
  gwidth = res * mt->metasize_x * grid->tile_sx;
  gheight = res * mt->metasize_y * grid->tile_sy;

  /* configured metatile size in geographical units */
  fullgwidth = res * tileset->metasize_x * grid->tile_sx;
  fullgheight = res * tileset->metasize_y * grid->tile_sy;
  
  switch(grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      mt->map.extent.minx = grid->extent.minx + mt->x * fullgwidth - gbuffer;
      mt->map.extent.miny = grid->extent.miny + mt->y * fullgheight - gbuffer;
      mt->map.extent.maxx = mt->map.extent.minx + gwidth + 2 * gbuffer;
      mt->map.extent.maxy = mt->map.extent.miny + gheight + 2 * gbuffer;
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      mt->map.extent.minx = grid->extent.minx + mt->x * fullgwidth - gbuffer;
      mt->map.extent.maxy = grid->extent.maxy - mt->y * fullgheight + gbuffer;
      mt->map.extent.maxx = mt->map.extent.minx + gwidth + 2 * gbuffer;
      mt->map.extent.miny = mt->map.extent.maxy - gheight - 2 * gbuffer;
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      ctx->set_error(ctx,500,"origin not implemented");
      return NULL;
  }

  for(i=0; i<mt->metasize_x; i++) {
    for(j=0; j<mt->metasize_y; j++) {
      mapcache_tile *t = &(mt->tiles[i*mt->metasize_y+j]);
      t->dimensions = tile->dimensions;
      t->grid_link = tile->grid_link;
      t->z = tile->z;
      t->x = blx + i;
      t->y = bly + j;
      t->tileset = tile->tileset;
    }
  }

  return mt;
}

/*
 * do the actual rendering and saving of a metatile:
 *  - query the datasource for the image data
 *  - split the resulting image along the metabuffer / metatiles
 *  - save each tile to cache
 */
void mapcache_tileset_render_metatile(mapcache_context *ctx, mapcache_metatile *mt)
{
  int i;
#ifdef DEBUG
  if(!mt->map.tileset->source) {
    ctx->set_error(ctx,500,"###BUG### tileset_render_metatile called on tileset with no source");
    return;
  }
#endif
  mt->map.tileset->source->render_map(ctx, &mt->map);
  GC_CHECK_ERROR(ctx);
  mapcache_image_metatile_split(ctx, mt);
  GC_CHECK_ERROR(ctx);
  if(mt->map.tileset->cache->tile_multi_set) {
    mt->map.tileset->cache->tile_multi_set(ctx, mt->tiles, mt->ntiles);
  } else {
    for(i=0; i<mt->ntiles; i++) {
      mapcache_tile *tile = &(mt->tiles[i]);
      mt->map.tileset->cache->tile_set(ctx, tile);
      GC_CHECK_ERROR(ctx);
    }
  }
}


/*
 * allocate and initialize a new tileset
 */
mapcache_tileset* mapcache_tileset_create(mapcache_context *ctx)
{
  mapcache_tileset* tileset = (mapcache_tileset*)apr_pcalloc(ctx->pool, sizeof(mapcache_tileset));
  tileset->metasize_x = tileset->metasize_y = 1;
  tileset->metabuffer = 0;
  tileset->expires = 300; /*set a reasonable default to 5 mins */
  tileset->auto_expire = 0;
  tileset->metadata = apr_table_make(ctx->pool,3);
  tileset->dimensions = NULL;
  tileset->format = NULL;
  tileset->grid_links = NULL;
  tileset->config = NULL;
  return tileset;
}

mapcache_tileset* mapcache_tileset_clone(mapcache_context *ctx, mapcache_tileset *src)
{
  mapcache_tileset* dst = (mapcache_tileset*)apr_pcalloc(ctx->pool, sizeof(mapcache_tileset));
  dst->metasize_x = src->metasize_x;
  dst->metasize_y = src->metasize_y;
  dst->metabuffer = src->metabuffer;
  dst->expires = src->expires;
  dst->auto_expire = src->auto_expire;
  dst->metadata = src->metadata;
  dst->dimensions = src->dimensions;
  dst->format = src->format;
  dst->grid_links = src->grid_links;
  dst->config = src->config;
  dst->name = src->name;
  dst->cache = src->cache;
  dst->source = src->source;
  dst->watermark = src->watermark;
  dst->wgs84bbox = src->wgs84bbox;
  dst->format = src->format;
  return dst;
}

/*
 * allocate and initialize a tile for a given tileset
 */
mapcache_tile* mapcache_tileset_tile_create(apr_pool_t *pool, mapcache_tileset *tileset, mapcache_grid_link *grid_link)
{
  mapcache_tile *tile = (mapcache_tile*)apr_pcalloc(pool, sizeof(mapcache_tile));
  tile->tileset = tileset;
  if(tileset->auto_expire) {
    tile->expires = tileset->auto_expire;
  } else {
    tile->expires = tileset->expires;
  }
  tile->grid_link = grid_link;
  if(tileset->dimensions) {
    int i;
    tile->dimensions = apr_table_make(pool,tileset->dimensions->nelts);
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      apr_table_set(tile->dimensions,dimension->name,dimension->default_value);
    }
  }
  return tile;
}

/*
 * allocate and initialize a map for a given tileset
 */
mapcache_map* mapcache_tileset_map_create(apr_pool_t *pool, mapcache_tileset *tileset, mapcache_grid_link *grid_link)
{
  mapcache_map *map = (mapcache_map*)apr_pcalloc(pool, sizeof(mapcache_map));
  map->tileset = tileset;
  map->grid_link = grid_link;
  if(tileset->dimensions) {
    int i;
    map->dimensions = apr_table_make(pool,tileset->dimensions->nelts);
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      apr_table_set(map->dimensions,dimension->name,dimension->default_value);
    }
  }
  return map;
}

/*
 * allocate and initialize a feature_info for a given tileset
 */
mapcache_feature_info* mapcache_tileset_feature_info_create(apr_pool_t *pool, mapcache_tileset *tileset,
    mapcache_grid_link *grid_link)
{
  mapcache_feature_info *fi = (mapcache_feature_info*)apr_pcalloc(pool, sizeof(mapcache_feature_info));
  fi->map.tileset = tileset;
  fi->map.grid_link = grid_link;
  if(tileset->dimensions) {
    int i;
    fi->map.dimensions = apr_table_make(pool,tileset->dimensions->nelts);
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      apr_table_set(fi->map.dimensions,dimension->name,dimension->default_value);
    }
  }
  return fi;
}

/**
 * \brief return the image data for a given tile
 * this call uses a global (interprocess+interthread) mutex if the tile was not found
 * in the cache.
 * the processing here is:
 *  - if the tile is found in the cache, return it. done
 *  - if it isn't found:
 *    - aquire mutex
 *    - check if the tile isn't being rendered by another thread/process
 *      - if another thread is rendering, wait for it to finish and return it's data
 *      - otherwise, lock all the tiles corresponding to the request (a metatile has multiple tiles)
 *    - release mutex
 *    - call the source to render the metatile, and save the tiles to disk
 *    - aquire mutex
 *    - unlock the tiles we have rendered
 *    - release mutex
 *
 */
void mapcache_tileset_tile_get(mapcache_context *ctx, mapcache_tile *tile)
{
  int isLocked,ret;
  mapcache_metatile *mt=NULL;
  ret = tile->tileset->cache->tile_get(ctx, tile);
  GC_CHECK_ERROR(ctx);

  if(ret == MAPCACHE_SUCCESS && tile->tileset->auto_expire && tile->mtime && tile->tileset->source) {
    /* the cache is in auto-expire mode, and can return the tile modification date,
     * and there is a source configured so we can possibly update it,
     * so we check to see if it is stale */
    apr_time_t now = apr_time_now();
    apr_time_t stale = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
    if(stale<now) {
      mapcache_tileset_tile_delete(ctx,tile,MAPCACHE_TRUE);
      GC_CHECK_ERROR(ctx);
      ret = MAPCACHE_CACHE_MISS;
    }
  }

  if(ret == MAPCACHE_CACHE_MISS) {

    /* bail out straight away if the tileset has no source */
    if(!tile->tileset->source) {
      /* there is no source configured for this tile. not an error, let caller now*/
      /*
      ctx->set_error(ctx,404,"tile not in cache, and no source configured for tileset %s",
            tile->tileset->name);
      */
      tile->nodata = 1;
      return;
    }

    /* bail out in non-blocking mode */
    if(ctx->config->non_blocking) {
      ctx->set_error(ctx,404,"tile not in cache, and configured for readonly mode");
      return;
    }

    /* the tile does not exist, we must take action before re-asking for it */
    /*
     * is the tile already being rendered by another thread ?
     * the call is protected by the same mutex that sets the lock on the tile,
     * so we can assure that:
     * - if the lock does not exist, then this thread should do the rendering
     * - if the lock exists, we should wait for the other thread to finish
     */

    /* aquire a lock on the metatile */
    mt = mapcache_tileset_metatile_get(ctx, tile);
    isLocked = mapcache_lock_or_wait_for_resource(ctx, mapcache_tileset_metatile_resource_key(ctx,mt));


    if(isLocked == MAPCACHE_TRUE) {
      /* no other thread is doing the rendering, do it ourselves */
#ifdef DEBUG
      ctx->log(ctx, MAPCACHE_DEBUG, "cache miss: tileset %s - tile %d %d %d",
               tile->tileset->name,tile->x, tile->y,tile->z);
#endif
      /* this will query the source to create the tiles, and save them to the cache */
      mapcache_tileset_render_metatile(ctx, mt);

      mapcache_unlock_resource(ctx, mapcache_tileset_metatile_resource_key(ctx,mt));
    }

    /* the previous step has successfully finished, we can now query the cache to return the tile content */
    ret = tile->tileset->cache->tile_get(ctx, tile);
    GC_CHECK_ERROR(ctx);

    if(ret != MAPCACHE_SUCCESS) {
      if(isLocked == MAPCACHE_FALSE) {
        ctx->set_error(ctx, 500, "tileset %s: unknown error (another thread/process failed to create the tile I was waiting for)",
                       tile->tileset->name);
      } else {
        /* shouldn't really happen, as the error ought to have been caught beforehand */
        ctx->set_error(ctx, 500, "tileset %s: failed to re-get tile %d %d %d from cache after set", tile->tileset->name,tile->x,tile->y,tile->z);
      }
    }
  }
  /* update the tile expiration time */
  if(tile->tileset->auto_expire && tile->mtime) {
    apr_time_t now = apr_time_now();
    apr_time_t expire_time = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
    tile->expires = apr_time_sec(expire_time-now);
  }
}

void mapcache_tileset_tile_delete(mapcache_context *ctx, mapcache_tile *tile, int whole_metatile)
{
  int i;
  /*delete the tile itself*/
  tile->tileset->cache->tile_delete(ctx,tile);
  GC_CHECK_ERROR(ctx);

  if(whole_metatile) {
    mapcache_metatile *mt = mapcache_tileset_metatile_get(ctx, tile);
    for(i=0; i<mt->ntiles; i++) {
      mapcache_tile *subtile = &mt->tiles[i];
      /* skip deleting the actual tile */
      if(subtile->x == tile->x && subtile->y == tile->y) continue;
      subtile->tileset->cache->tile_delete(ctx,subtile);
      /* silently pass failure if the tile was not found */
      if(ctx->get_error(ctx) == 404) {
        ctx->clear_errors(ctx);
      }
      GC_CHECK_ERROR(ctx);
    }
  }
}


/* vim: ts=2 sts=2 et sw=2
*/
