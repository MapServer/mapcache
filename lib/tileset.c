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

char* mapcache_tileset_tile_resource_key(mapcache_context *ctx, mapcache_tile *tile) {
  char *lockname = apr_psprintf(ctx->pool,
                                "%d-%d-%d-%s",
                                tile->z,tile->y/tile->tileset->metasize_y,tile->x/tile->tileset->metasize_x,
                                tile->tileset->name);

  /* if the tileset has multiple grids, add the name of the current grid to the lock key*/
  if(tile->tileset->grid_links->nelts > 1) {
    lockname = apr_pstrcat(ctx->pool,lockname,tile->grid_link->grid->name,NULL);
  }

  if(tile->dimensions && tile->dimensions->nelts>0) {
    int i;
    for(i=0; i<tile->dimensions->nelts; i++) {
      mapcache_requested_dimension *rdim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
      char *dimvalue = apr_pstrdup(ctx->pool,rdim->cached_value);
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

char* mapcache_tileset_metatile_resource_key(mapcache_context *ctx, mapcache_metatile *mt)
{
  return mapcache_tileset_tile_resource_key(ctx,&mt->tiles[0]);
}

void mapcache_tileset_configuration_check(mapcache_context *ctx, mapcache_tileset *tileset)
{
  /* check we have all we want */
  if(tileset->_cache == NULL) {
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

  if(tileset->format && tileset->format->type == GC_RAW) {
    if(tileset->metasize_x != 1 || tileset->metasize_y != 1 || tileset->metabuffer != 0) {
      ctx->set_error(ctx, 400, "tileset \"%s\" references a RAW format type, metatiling is not supported for the \"%s\" format", tileset->name, tileset->format->name);
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

void mapcache_tileset_tile_validate_z(mapcache_context *ctx, mapcache_tile *tile) {
  if(tile->z < tile->grid_link->minz || tile->z >= tile->grid_link->maxz) {
    ctx->set_error(ctx,404,"invalid tile z level");
  }
}

void mapcache_tileset_tile_validate_x(mapcache_context *ctx, mapcache_tile *tile) {
  mapcache_extent_i limits;
  limits = tile->grid_link->grid_limits[tile->z];
  if(tile->x<limits.minx || tile->x>=limits.maxx) {
    ctx->set_error(ctx, 404, "tile x=%d not in [%d,%d[",
                   tile->x,limits.minx,limits.maxx);
  }
}

void mapcache_tileset_tile_validate_y(mapcache_context *ctx, mapcache_tile *tile) {
  mapcache_extent_i limits;
  limits = tile->grid_link->grid_limits[tile->z];
  if(tile->y<limits.miny || tile->y>=limits.maxy) {
    ctx->set_error(ctx, 404, "tile y=%d not in [%d,%d[",
                   tile->y,limits.miny,limits.maxy);
  }
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
                                    mapcache_tile ***tiles,
                                    mapcache_grid_link **effectively_used_grid_link)
{
  double resolution;
  int level;
  int bl_x,bl_y,tr_x,tr_y;
  int mx,my,Mx,My;
  int x,y;
  int i=0;
  resolution = mapcache_grid_get_resolution(bbox, width, height);
  *effectively_used_grid_link = mapcache_grid_get_closest_wms_level(ctx,grid_link,resolution,&level);

  /* we don't want to assemble tiles that have already been reassembled from a lower level */
  if((*effectively_used_grid_link)->outofzoom_strategy == MAPCACHE_OUTOFZOOM_REASSEMBLE && level > (*effectively_used_grid_link)->max_cached_zoom) {
    level = (*effectively_used_grid_link)->max_cached_zoom;
  }

  mapcache_grid_get_xy(ctx,(*effectively_used_grid_link)->grid,bbox->minx,bbox->miny,level,&bl_x,&bl_y);
  mapcache_grid_get_xy(ctx,(*effectively_used_grid_link)->grid,bbox->maxx,bbox->maxy,level,&tr_x,&tr_y);
  Mx = MAPCACHE_MAX(MAPCACHE_MIN(MAPCACHE_MAX(tr_x,bl_x),(*effectively_used_grid_link)->grid_limits[level].maxx),(*effectively_used_grid_link)->grid_limits[level].minx);
  My = MAPCACHE_MAX(MAPCACHE_MIN(MAPCACHE_MAX(tr_y,bl_y),(*effectively_used_grid_link)->grid_limits[level].maxy),(*effectively_used_grid_link)->grid_limits[level].miny);
  mx = MAPCACHE_MIN(MAPCACHE_MAX(MAPCACHE_MIN(tr_x,bl_x),(*effectively_used_grid_link)->grid_limits[level].minx),(*effectively_used_grid_link)->grid_limits[level].maxx);
  my = MAPCACHE_MIN(MAPCACHE_MAX(MAPCACHE_MIN(tr_y,bl_y),(*effectively_used_grid_link)->grid_limits[level].miny),(*effectively_used_grid_link)->grid_limits[level].maxy);
  *ntiles = (Mx-mx+1)*(My-my+1);
  i=0;
  *tiles = (mapcache_tile**)apr_pcalloc(ctx->pool, *ntiles*sizeof(mapcache_tile*));
  for(x=mx; x<=Mx; x++) {
    for(y=my; y<=My; y++) {
      mapcache_tile *tile = mapcache_tileset_tile_create(ctx->pool,tileset, (*effectively_used_grid_link));
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
  mapcache_image *image;
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

  image = mapcache_image_create_with_data(ctx,width,height);
  if(ntiles == 0) {
    image->has_alpha = MC_ALPHA_YES;
    image->is_blank = MC_EMPTY_YES;
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
  srcimage = mapcache_image_create_with_data(ctx,
          (Mx-mx+1)*tiles[0]->grid_link->grid->tile_sx,
          (My-my+1)*tiles[0]->grid_link->grid->tile_sy);

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
      default:
        ctx->set_error(ctx,500,"BUG: invalid grid origin");
        return NULL;
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
  mapcache_grid_get_tile_extent(ctx,toplefttile->grid_link->grid,
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
        mapcache_image_copy_resampled_bilinear(ctx,srcimage,image,dstminx,dstminy,hf,vf,0);
        break;
      default:
        mapcache_image_copy_resampled_nearest(ctx,srcimage,image,dstminx,dstminy,hf,vf);
        break;
    }
  }
  /* free the memory of the temporary source image */
  apr_pool_cleanup_run(ctx->pool, srcimage->data, (void*)free) ;
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
  mapcache_tileset *tileset = mt->map.tileset;

  if(!tileset->source || tileset->read_only) {
    ctx->set_error(ctx,500,"tileset_render_metatile called on tileset with no source or that is read-only");
    return;
  }
  mapcache_source_render_map(ctx, tileset->source, &mt->map);
  GC_CHECK_ERROR(ctx);
  mapcache_image_metatile_split(ctx, mt);
  GC_CHECK_ERROR(ctx);
  mapcache_cache_tile_multi_set(ctx, tileset->_cache, mt->tiles, mt->ntiles);
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
  tileset->read_only = 0;
  tileset->metadata = apr_table_make(ctx->pool,3);
  tileset->dimensions = NULL;
  tileset->format = NULL;
  tileset->grid_links = NULL;
  tileset->config = NULL;
  tileset->store_dimension_assemblies = 1;
  tileset->dimension_assembly_type = MAPCACHE_DIMENSION_ASSEMBLY_NONE;
  tileset->subdimension_read_only = 0;
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
  dst->_cache = src->_cache;
  dst->source = src->source;
  dst->watermark = src->watermark;
  dst->wgs84bbox = src->wgs84bbox;
  dst->format = src->format;
  dst->store_dimension_assemblies = src->store_dimension_assemblies;
  dst->dimension_assembly_type = src->dimension_assembly_type;
  dst->subdimension_read_only = src->subdimension_read_only;
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
    tile->dimensions = apr_array_make(pool,tileset->dimensions->nelts,sizeof(mapcache_requested_dimension*));
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      mapcache_requested_dimension *rdim = apr_pcalloc(pool,sizeof(mapcache_requested_dimension));
      rdim->requested_value = dimension->default_value;
      rdim->cached_value = NULL;
      rdim->dimension = dimension;
      APR_ARRAY_PUSH(tile->dimensions,mapcache_requested_dimension*) = rdim;
    }
  }
  return tile;
}

mapcache_tile* mapcache_tileset_tile_clone(apr_pool_t *pool, mapcache_tile *src)
{
  mapcache_tile *tile = (mapcache_tile*)apr_pcalloc(pool, sizeof(mapcache_tile));
  tile->tileset = src->tileset;
  tile->expires = src->expires;
  tile->grid_link = src->grid_link;
  tile->dimensions = mapcache_requested_dimensions_clone(pool, src->dimensions);
  tile->x = src->x;
  tile->y = src->y;
  tile->z = src->z;
  tile->allow_redirect = src->allow_redirect;
  return tile;
}

mapcache_map* mapcache_tileset_map_clone(apr_pool_t *pool, mapcache_map *src)
{
  mapcache_map *map = (mapcache_map*)apr_pcalloc(pool, sizeof(mapcache_map));
  map->tileset = src->tileset;
  map->expires = src->expires;
  map->grid_link = src->grid_link;
  map->dimensions = mapcache_requested_dimensions_clone(pool, src->dimensions);
  map->height = src->height;
  map->width = src->width;
  map->extent = src->extent;
  return map;
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
    map->dimensions = apr_array_make(pool,tileset->dimensions->nelts,sizeof(mapcache_requested_dimension*));
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      mapcache_requested_dimension *rdim = apr_pcalloc(pool,sizeof(mapcache_requested_dimension));
      rdim->requested_value = dimension->default_value;
      rdim->cached_value = NULL;
      rdim->dimension = dimension;
      APR_ARRAY_PUSH(map->dimensions,mapcache_requested_dimension*) = rdim;
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
    fi->map.dimensions = apr_array_make(pool,tileset->dimensions->nelts,sizeof(mapcache_requested_dimension*));
    for(i=0; i<tileset->dimensions->nelts; i++) {
      mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
      mapcache_requested_dimension *rdim = apr_pcalloc(pool,sizeof(mapcache_requested_dimension));
      rdim->requested_value = dimension->default_value;
      rdim->cached_value = NULL;
      rdim->dimension = dimension;
      APR_ARRAY_PUSH(fi->map.dimensions,mapcache_requested_dimension*) = rdim;
    }
  }
  return fi;
}

void mapcache_tileset_assemble_out_of_zoom_tile(mapcache_context *ctx, mapcache_tile *tile) {
  mapcache_extent tile_bbox;
  double shrink_x, shrink_y, scalefactor;
  int x[4],y[4];
  int i, n=1;
  mapcache_tile *childtile;
  assert(tile->grid_link->outofzoom_strategy == MAPCACHE_OUTOFZOOM_REASSEMBLE);

  /* we have at most 4 tiles composing the requested tile */
  mapcache_grid_get_tile_extent(ctx,tile->grid_link->grid,tile->x,tile->y,tile->z, &tile_bbox);

  /*
   shrink the extent so we do not fall exactly on a tile boundary, to avoid rounding
   errors when computing the x,y of the lower level tile(s) we will need
  */

  shrink_x = (tile_bbox.maxx - tile_bbox.minx) / (tile->grid_link->grid->tile_sx * 1000); /* 1/1000th of a pixel */
  shrink_y = (tile_bbox.maxy - tile_bbox.miny) / (tile->grid_link->grid->tile_sy * 1000); /* 1/1000th of a pixel */
  tile_bbox.maxx -= shrink_x;
  tile_bbox.maxy -= shrink_y;
  tile_bbox.minx += shrink_x;
  tile_bbox.miny += shrink_y;

  /* compute the x,y of the lower level tiles we'll use for reassembling (we take them from the grid_link->max_cached_zoom,
   * which is the closest level were we can consume tiles from the cache
   */

  mapcache_grid_get_xy(ctx,tile->grid_link->grid,tile_bbox.minx, tile_bbox.miny, tile->grid_link->max_cached_zoom, &x[0], &y[0]);
  mapcache_grid_get_xy(ctx,tile->grid_link->grid,tile_bbox.maxx, tile_bbox.maxy, tile->grid_link->max_cached_zoom, &x[1], &y[1]);
  if(x[0] != x[1] || y[0] != y[1]) {
    /* no use computing these if the first two were identical */
    n = 4;
    mapcache_grid_get_xy(ctx,tile->grid_link->grid,tile_bbox.minx, tile_bbox.maxy, tile->grid_link->max_cached_zoom, &x[2], &y[2]);
    mapcache_grid_get_xy(ctx,tile->grid_link->grid,tile_bbox.maxx, tile_bbox.miny, tile->grid_link->max_cached_zoom, &x[3], &y[3]);
  }
  tile_bbox.maxx += shrink_x;
  tile_bbox.maxy += shrink_y;
  tile_bbox.minx -= shrink_x;
  tile_bbox.miny -= shrink_y;

  childtile = mapcache_tileset_tile_clone(ctx->pool,tile);
  childtile->z = tile->grid_link->max_cached_zoom;
  scalefactor = childtile->grid_link->grid->levels[childtile->z]->resolution/tile->grid_link->grid->levels[tile->z]->resolution;
  tile->nodata = 1;
  for(i=0;i<n;i++) {
    mapcache_extent childtile_bbox;
    double dstminx,dstminy;
    childtile->x = x[i];
    childtile->y = y[i];
    mapcache_tileset_tile_get(ctx,childtile);
    GC_CHECK_ERROR(ctx);
    if(childtile->nodata) {
      /* silently skip empty tiles */
      childtile->nodata = 0; /* reset flag */
      continue;
    }
    if(!childtile->raw_image) {
      childtile->raw_image = mapcache_imageio_decode(ctx, childtile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    if(tile->nodata) {
      /* we defer the creation of the actual image bytes, no use allocating before knowing
       that one of the child tiles actually contains data*/
      tile->raw_image = mapcache_image_create_with_data(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy);
      tile->nodata = 0;
    }
    /* now copy/scale the srcimage onto the destination image */
    mapcache_grid_get_tile_extent(ctx,childtile->grid_link->grid,
                            childtile->x, childtile->y, childtile->z, &childtile_bbox);

    /*compute the pixel position of top left corner*/
    dstminx = (childtile_bbox.minx-tile_bbox.minx)/tile->grid_link->grid->levels[tile->z]->resolution;
    dstminy = (tile_bbox.maxy-childtile_bbox.maxy)/tile->grid_link->grid->levels[tile->z]->resolution;
    /*
     * ctx->log(ctx, MAPCACHE_DEBUG, "factor: %g. start: %g,%g (im size: %g)",scalefactor,dstminx,dstminy,scalefactor*256);
     */
    if(scalefactor <= tile->grid_link->grid->tile_sx/2) /*FIXME: might fail for non-square tiles, also check tile_sy */
      mapcache_image_copy_resampled_bilinear(ctx,childtile->raw_image,tile->raw_image,dstminx,dstminy,scalefactor,scalefactor,1);
    else {
      /* no use going through bilinear resampling if the requested scalefactor maps less than 4 pixels onto the
      * resulting tile, plus pixman has some rounding bugs in this case, see
      * https://bugs.freedesktop.org/show_bug.cgi?id=46277 */
      unsigned int row,col;
      unsigned char *srcpixptr;
      unsigned char *row_ptr;
      unsigned int dstminxi = - dstminx / scalefactor;
      unsigned int dstminyi = - dstminy / scalefactor;
      srcpixptr = &(childtile->raw_image->data[dstminyi * childtile->raw_image->stride + dstminxi * 4]);
      /*
      ctx->log(ctx, MAPCACHE_WARN, "factor: %g. pixel: %d,%d (val:%d)",scalefactor,dstminxi,dstminyi,*((unsigned int*)srcpixptr));
       */
      row_ptr = tile->raw_image->data;
      for(row=0;row<tile->raw_image->h;row++) {
        unsigned char *pix_ptr = row_ptr;
        for(col=0;col<tile->raw_image->w;col++) {
          *((unsigned int*)pix_ptr) = *((unsigned int*)srcpixptr);
          pix_ptr += 4;
        }
        row_ptr += tile->raw_image->stride;
      }
    }


    /* do some cleanup, a bit in advance as we won't be using this tile's data anymore */
    apr_pool_cleanup_run(ctx->pool,childtile->raw_image->data,(void*)free);
    childtile->raw_image = NULL;
    childtile->encoded_data = NULL;
  }
}

void mapcache_tileset_outofzoom_get(mapcache_context *ctx, mapcache_tile *tile) {
  assert(tile->grid_link->outofzoom_strategy != MAPCACHE_OUTOFZOOM_NOTCONFIGURED);
  if(tile->grid_link->outofzoom_strategy == MAPCACHE_OUTOFZOOM_REASSEMBLE) {
    mapcache_tileset_assemble_out_of_zoom_tile(ctx, tile);
  } else {/* if(tile->grid_link->outofzoom_strategy == MAPCACHE_OUTOFZOOM_PROXY) */
    if(ctx->config->non_blocking) {
      ctx->set_error(ctx,404,"cannot proxy out-of-zoom tile, I'm configured in non-blocking mode");
      return;
    }
    ctx->set_error(ctx,500,"Proxying out of zoom tiles not implemented");
  }
}

int mapcache_tileset_tile_get_readonly(mapcache_context *ctx, mapcache_tile *tile) {
  int ret = mapcache_cache_tile_get(ctx, tile->tileset->_cache, tile);
  if(GC_HAS_ERROR(ctx))
    return ret;
  
  if(ret == MAPCACHE_SUCCESS && tile->tileset->auto_expire && tile->mtime && tile->tileset->source && !tile->tileset->read_only) {
    /* the cache is in auto-expire mode, and can return the tile modification date,
     * and there is a source configured so we can possibly update it,
     * so we check to see if it is stale */
    apr_time_t now = apr_time_now();
    apr_time_t stale = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
    if(stale<now) {
      mapcache_tileset_tile_delete(ctx,tile,0);
      if(ctx->get_error(ctx) == 404) {
        ctx->clear_errors(ctx);
      }
      ret = MAPCACHE_CACHE_MISS;
    }
  }
  return ret;
}

typedef struct {
  mapcache_tile *tile;
  int cache_status;
} mapcache_subtile;

static void mapcache_tileset_tile_get_without_subdimensions(mapcache_context *ctx, mapcache_tile *tile, int read_only);

void mapcache_tileset_tile_set_get_with_subdimensions(mapcache_context *ctx, mapcache_tile *tile) {
  apr_array_header_t *subtiles;
  mapcache_extent extent;
  mapcache_subtile st;
  mapcache_image *assembled_image = NULL;
  mapcache_buffer *assembled_buffer = NULL;
  int i,j,k,n_subtiles = 1,assembled_nodata = 1;
  /* we can be here in two cases:
   * - either we didn't look up the tile directly (need to split dimension into sub-dimension and reassemble dynamically)
   * - either the direct lookup failed and we need to render/assemble the tiles from subdimensions
   */
  subtiles = apr_array_make(ctx->pool,1,sizeof(mapcache_subtile));
  st.tile = tile;
  APR_ARRAY_PUSH(subtiles,mapcache_subtile) = st;
  mapcache_grid_get_tile_extent(ctx,tile->grid_link->grid,tile->x,tile->y,tile->z,&extent);
  if(GC_HAS_ERROR(ctx)) goto cleanup;

  for(i=0;i<tile->dimensions->nelts; i++) {
    mapcache_requested_dimension *rdim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
    apr_array_header_t *single_subdimension = mapcache_dimension_get_entries_for_value(ctx,rdim->dimension,rdim->requested_value,
                                                                                     tile->tileset, &extent, tile->grid_link->grid);
    if(GC_HAS_ERROR(ctx)) /* invalid dimension given */
      goto cleanup;
#ifdef DEBUG
    {
      char *dims = "";
      int i;
      for(i=0;i<single_subdimension->nelts;i++)
        dims = apr_pstrcat(ctx->pool,dims,APR_ARRAY_IDX(single_subdimension,i,char*)," ",NULL);
      ctx->log(ctx,MAPCACHE_DEBUG,"tile (%d,%d,%d) dimension (%s) returned: %s",
             tile->z,tile->y,tile->x,rdim->dimension->name,dims);
    }
#endif

    if(single_subdimension->nelts == 0) {
      /* not an error, but no subdimension was found: we need to return an empty tile */
      tile->nodata = 1;
      if(tile->tileset->store_dimension_assemblies) {
        tile->raw_image = mapcache_image_create_with_data(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy);
        tile->raw_image->has_alpha = MC_ALPHA_YES;
        tile->raw_image->is_blank = MC_EMPTY_YES;
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        /* set the key for the dimension so it can be stored with the requested dimension */
        for(j=0;j<tile->dimensions->nelts;j++) {
          mapcache_requested_dimension *dim = APR_ARRAY_IDX(tile->dimensions,j,mapcache_requested_dimension*);
          dim->cached_value = dim->requested_value;
        }
        mapcache_cache_tile_set(ctx, tile->tileset->_cache, tile);
        GC_CHECK_ERROR(ctx);
      }
      return;
    } else {
      for(j=0;j<n_subtiles;j++) {
        /* clone the existing subtiles if we have more than one sub-dimension to assemble for the the current dimension */
        for(k=1;k<single_subdimension->nelts;k++) {
          st.tile = mapcache_tileset_tile_clone(ctx->pool,APR_ARRAY_IDX(subtiles,j,mapcache_subtile).tile);
          APR_ARRAY_PUSH(subtiles,mapcache_subtile)=st;
        }
      }
      n_subtiles *= single_subdimension->nelts;
      /* foreach of the subtiles, now set the actual subdimension we are going to be using
         the "j%nelts" part takes care of looping over and over through the individual subdimensions */
      for(j=0;j<n_subtiles;j++) {
        mapcache_tile_set_cached_dimension(ctx,APR_ARRAY_IDX(subtiles,j,mapcache_subtile).tile,rdim->dimension->name,
                                           APR_ARRAY_IDX(single_subdimension,j%single_subdimension->nelts,char*));

      }
    }
  }

  /* our subtiles array now contains a list of tiles with subdimensions split up, we now need to fetch them from the cache */
  /* note that subtiles[0].tile == tile */

  for(i=subtiles->nelts-1; i>=0; i--) {
    mapcache_tile *subtile = APR_ARRAY_IDX(subtiles,i,mapcache_subtile).tile;
    mapcache_tileset_tile_get_without_subdimensions(ctx, subtile, (tile->tileset->subdimension_read_only||!tile->tileset->source)?1:0); /* creates the tile from the source, takes care of metatiling */
    if(GC_HAS_ERROR(ctx))
      goto cleanup;
    if(!subtile->nodata) {
      assembled_nodata = 0;
      if(!assembled_buffer && !assembled_image) {
        /* first "usable" subtile */
        assembled_buffer = subtile->encoded_data;
        assembled_image = subtile->raw_image;
      } else {
        /* need to merge current assembled tile over this subtile */
        if(!assembled_image) {
          assembled_image = mapcache_imageio_decode(ctx,assembled_buffer);
          if(GC_HAS_ERROR(ctx))
            goto cleanup;
          assembled_buffer = NULL; /* the image data went stale as we're merging something */
        }
        if(!subtile->raw_image) {
          subtile->raw_image = mapcache_imageio_decode(ctx,subtile->encoded_data);
          if(GC_HAS_ERROR(ctx))
            goto cleanup;
        }
        mapcache_image_merge(ctx, subtile->raw_image, assembled_image);
        assembled_image = subtile->raw_image;
        assembled_image->has_alpha = MC_ALPHA_UNKNOWN; /* we've merged two images, we now have no idea if it's transparent or not */
        if(GC_HAS_ERROR(ctx))
          goto cleanup;
      }
      if((subtile->encoded_data && mapcache_imageio_header_sniff(ctx,subtile->encoded_data) == GC_JPEG)||
         (subtile->raw_image && subtile->raw_image->has_alpha == MC_ALPHA_NO)) {
        /* the returned image is fully opaque, we don't need to get/decode/merge any further subtiles */
        if(assembled_image)
          assembled_image->has_alpha = MC_ALPHA_NO;
        break;
      }
    }
  }
  
  tile->encoded_data = assembled_buffer;
  tile->raw_image = assembled_image;
  tile->nodata = assembled_nodata;

  /* TODO: how should the no data case be handled generically?
   * uncomment the following block if this nodata state should be returned to
   * the requester immediately, without this info being stored to the cache. 
   * Leaving this uncommented will cause a no-data tile to be (maybe, depending
   * on the cache's actual configuration) written to the cache
   */
  /*
  if(tile->nodata) {
    goto cleanup;
  }
  */

  if(!tile->nodata && !tile->encoded_data) {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }
  if(tile->tileset->store_dimension_assemblies) {
    int already_stored = 1; /*depending on the type of dimension, we may have no nead to store the resulting tile*/

    if(n_subtiles != 1)
      already_stored = 0; /*if we had to merge multiple subdimensions, then we always have to store the resulting assembly*/

    /* set the key for the dimension so it can be stored with the requested dimension */
    for(j=0;j<tile->dimensions->nelts;j++) {
      mapcache_requested_dimension *dim = APR_ARRAY_IDX(tile->dimensions,j,mapcache_requested_dimension*);
      if(strcmp(dim->cached_value,dim->requested_value)) {
        already_stored = 0; /*the subdimension is different than the requested dimension, we need to store the resulting tile*/
      }
      dim->cached_value = dim->requested_value;
    }
    if(!already_stored) {
      if(tile->nodata) {
        tile->raw_image = mapcache_image_create_with_data(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy);
        tile->raw_image->has_alpha = MC_ALPHA_YES;
        tile->raw_image->is_blank = MC_EMPTY_YES;
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        GC_CHECK_ERROR(ctx);
      }
      mapcache_cache_tile_set(ctx, tile->tileset->_cache, tile);
      GC_CHECK_ERROR(ctx);
    }
  }

cleanup:
  return;
}

void mapcache_tileset_tile_get_with_subdimensions(mapcache_context *ctx, mapcache_tile *tile) {
  int i,ret;

  assert(tile->dimensions);
  if(tile->tileset->store_dimension_assemblies) {
    for(i=0;i<tile->dimensions->nelts;i++) {
      mapcache_requested_dimension *dim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
      dim->cached_value = dim->requested_value;
    }
    ret = mapcache_tileset_tile_get_readonly(ctx,tile);
    GC_CHECK_ERROR(ctx);
    if(ret == MAPCACHE_SUCCESS) {
      /* update the tile expiration time */
      if(tile->tileset->auto_expire && tile->mtime) {
        apr_time_t now = apr_time_now();
        apr_time_t expire_time = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
        tile->expires = apr_time_sec(expire_time-now);
      }
      return;
    }
    for(i=0;i<tile->dimensions->nelts;i++) {
      /* unset the cached dimension we setup earlier on */
      mapcache_requested_dimension *dim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
      dim->cached_value = NULL;
    }
  }
  return mapcache_tileset_tile_set_get_with_subdimensions(ctx,tile);
  
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

static void mapcache_tileset_tile_get_without_subdimensions(mapcache_context *ctx, mapcache_tile *tile, int read_only)
{
  int ret;
  mapcache_metatile *mt=NULL;
  ret = mapcache_cache_tile_get(ctx, tile->tileset->_cache, tile);
  GC_CHECK_ERROR(ctx);

  if(ret == MAPCACHE_SUCCESS && tile->tileset->auto_expire && tile->mtime && tile->tileset->source && !tile->tileset->read_only) {
    /* the cache is in auto-expire mode, and can return the tile modification date,
     * and there is a source configured so we can possibly update it,
     * so we check to see if it is stale */
    apr_time_t now = apr_time_now();
    apr_time_t stale = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
    if(stale<now) {
      /* Indicate that we need to re-render the tile */
      ret = MAPCACHE_CACHE_RELOAD;
    }
  }

  if (ret == MAPCACHE_CACHE_MISS) {
    /* bail out straight away if the tileset has no source or is read-only */
    if(read_only) {
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
  }

  if (ret == MAPCACHE_CACHE_MISS || ret == MAPCACHE_CACHE_RELOAD) {
    int isLocked = MAPCACHE_FALSE;
    void *lock;

    /* If the tile does not exist or stale, we must take action before re-asking for it */
    if( !read_only && !ctx->config->non_blocking) {
      /*
       * is the tile already being rendered by another thread ?
       * the call is protected by the same mutex that sets the lock on the tile,
       * so we can assure that:
       * - if the lock does not exist, then this thread should do the rendering
       * - if the lock exists, we should wait for the other thread to finish
       */

      /* aquire a lock on the metatile */
      mt = mapcache_tileset_metatile_get(ctx, tile);
      isLocked = mapcache_lock_or_wait_for_resource(ctx, ctx->config->locker, mapcache_tileset_metatile_resource_key(ctx,mt), &lock);
      GC_CHECK_ERROR(ctx);
      if(isLocked == MAPCACHE_TRUE) {
         /* no other thread is doing the rendering, do it ourselves */
#ifdef DEBUG
        ctx->log(ctx, MAPCACHE_DEBUG, "cache miss/reload: tileset %s - tile %d %d %d",
             tile->tileset->name,tile->x, tile->y,tile->z);
#endif
        /* this will query the source to create the tiles, and save them to the cache */
        mapcache_tileset_render_metatile(ctx, mt);

        if(GC_HAS_ERROR(ctx)) {
          /* temporarily clear error state so we don't mess up with error handling in the locker */
          void *error;
          ctx->pop_errors(ctx,&error);
          mapcache_unlock_resource(ctx, ctx->config->locker, lock);
          ctx->push_errors(ctx,error);
        } else {
          mapcache_unlock_resource(ctx, ctx->config->locker, lock);
        }
      }
    }

    if (ret == MAPCACHE_CACHE_RELOAD && GC_HAS_ERROR(ctx))
      /* If we tried to reload a stale tile but failed, we know we have already
       * fetched it from the cache. We can then ignore errors and just use old tile.
       */
      ctx->clear_errors(ctx);

    else {
      /* Else, check for errors and try to fetch the tile from the cache.
      */
      GC_CHECK_ERROR(ctx);
      ret = mapcache_cache_tile_get(ctx, tile->tileset->_cache, tile);
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
  }
  /* update the tile expiration time */
  if(tile->tileset->auto_expire && tile->mtime) {
    apr_time_t now = apr_time_now();
    apr_time_t expire_time = tile->mtime + apr_time_from_sec(tile->tileset->auto_expire);
    tile->expires = apr_time_sec(expire_time-now);
  }
}

void mapcache_tileset_tile_get(mapcache_context *ctx, mapcache_tile *tile) {
  if(tile->grid_link->outofzoom_strategy != MAPCACHE_OUTOFZOOM_NOTCONFIGURED &&
          tile->z > tile->grid_link->max_cached_zoom) {
    mapcache_tileset_outofzoom_get(ctx, tile);
    return;
  }
  if(tile->dimensions) {
    if(tile->tileset->dimension_assembly_type != MAPCACHE_DIMENSION_ASSEMBLY_NONE) {
      return mapcache_tileset_tile_get_with_subdimensions(ctx,tile);
    } else {
      int i;
      mapcache_requested_dimension *rdim;
      mapcache_extent extent;
      
      mapcache_grid_get_tile_extent(ctx,tile->grid_link->grid,tile->x,tile->y,tile->z,&extent);
      for(i=0; i<tile->dimensions->nelts; i++) {
        apr_array_header_t *rdim_vals;
        rdim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
        rdim_vals = mapcache_dimension_get_entries_for_value(ctx,rdim->dimension,rdim->requested_value, tile->tileset, NULL, tile->grid_link->grid);
        GC_CHECK_ERROR(ctx);
        if(rdim_vals->nelts > 1) {
          ctx->set_error(ctx,500,"dimension (%s) for tileset (%s) returned invalid number (%d) of subdimensions (1 expected)",
                         rdim->dimension->name, tile->tileset->name, rdim_vals->nelts);
          return;
        }
        if(rdim_vals->nelts == 0) {
          ctx->set_error(ctx,404,"dimension (%s) for tileset (%s) returned no subdimensions (1 expected)",rdim->dimension->name, tile->tileset->name);
          return;
        }
        rdim->cached_value = APR_ARRAY_IDX(rdim_vals,0,char*);
      }
    }
  }
  return mapcache_tileset_tile_get_without_subdimensions(ctx,tile, (tile->tileset->read_only||!tile->tileset->source)?1:0);
  
}

void mapcache_tileset_tile_delete(mapcache_context *ctx, mapcache_tile *tile, int whole_metatile)
{
  int i;
  /*delete the tile itself*/
  mapcache_cache_tile_delete(ctx,tile->tileset->_cache, tile);
  GC_CHECK_ERROR(ctx);

  if(whole_metatile) {
    mapcache_metatile *mt = mapcache_tileset_metatile_get(ctx, tile);
    for(i=0; i<mt->ntiles; i++) {
      mapcache_tile *subtile = &mt->tiles[i];
      /* skip deleting the actual tile */
      if(subtile->x == tile->x && subtile->y == tile->y) continue;
      mapcache_cache_tile_delete(ctx,subtile->tileset->_cache,subtile);
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
