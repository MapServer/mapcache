/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching grid support file
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
#include <math.h>
#include <apr_strings.h>
/*
 * allocate and initialize a new tileset
 */
mapcache_grid* mapcache_grid_create(apr_pool_t *pool)
{
  mapcache_grid* grid = (mapcache_grid*)apr_pcalloc(pool, sizeof(mapcache_grid));
  grid->metadata = apr_table_make(pool,3);
  grid->srs_aliases = apr_array_make(pool,0,sizeof(char*));
  grid->unit = MAPCACHE_UNIT_METERS;
  grid->origin = MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT;
  return grid;
}


/**
 * \brief compute the extent of a given tile in the grid given its x, y, and z.
 * \returns \extent the tile's extent
 */
void mapcache_grid_get_extent(mapcache_context *ctx, mapcache_grid *grid,
                              int x, int y, int z, mapcache_extent *bbox)
{
  double res  = grid->levels[z]->resolution;
  switch(grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      bbox->minx = grid->extent.minx + (res * x * grid->tile_sx);
      bbox->miny = grid->extent.miny + (res * y * grid->tile_sy);
      bbox->maxx = grid->extent.minx + (res * (x + 1) * grid->tile_sx);
      bbox->maxy = grid->extent.miny + (res * (y + 1) * grid->tile_sy);
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      bbox->minx = grid->extent.minx + (res * x * grid->tile_sx);
      bbox->miny = grid->extent.maxy - (res * (y+1) * grid->tile_sy);
      bbox->maxx = grid->extent.minx + (res * (x + 1) * grid->tile_sx);
      bbox->maxy = grid->extent.maxy - (res * y * grid->tile_sy);
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      ctx->set_error(ctx,500,"grid origin not implemented");
  }
}

const char* mapcache_grid_get_crs(mapcache_context *ctx, mapcache_grid *grid)
{
  char *epsgnum;

  /*locate the number after epsg: in the grd srs*/
  epsgnum = strchr(grid->srs,':');
  if(!epsgnum) {
    epsgnum = grid->srs;
  } else {
    epsgnum++;
  }

  return apr_psprintf(ctx->pool,"urn:ogc:def:crs:EPSG:6.3:%s",epsgnum);
}

const char* mapcache_grid_get_srs(mapcache_context *ctx, mapcache_grid *grid)
{
  return (const char*)grid->srs;
}

void mapcache_grid_compute_limits(const mapcache_grid *grid, const mapcache_extent *extent, mapcache_extent_i *limits, int tolerance)
{
  int i;
  double epsilon = 0.0000001;
  for(i=0; i<grid->nlevels; i++) {
    mapcache_grid_level *level = grid->levels[i];
    double unitheight = grid->tile_sy * level->resolution;
    double unitwidth = grid->tile_sx * level->resolution;

    switch(grid->origin) {
      case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
        limits[i].minx = floor((extent->minx - grid->extent.minx) / unitwidth + epsilon) - tolerance;
        limits[i].maxx = ceil((extent->maxx - grid->extent.minx) / unitwidth - epsilon) + tolerance;
        limits[i].miny = floor((extent->miny - grid->extent.miny) / unitheight + epsilon) - tolerance;
        limits[i].maxy = ceil((extent->maxy - grid->extent.miny) / unitheight - epsilon) + tolerance;
        break;
      case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
        limits[i].minx = floor((extent->minx - grid->extent.minx) / unitwidth + epsilon) - tolerance;
        limits[i].maxx = ceil((extent->maxx - grid->extent.minx) / unitwidth - epsilon) + tolerance;
        limits[i].miny = floor((grid->extent.maxy - extent->maxy) / unitheight + epsilon) - tolerance;
        //limits[i].maxy = level->maxy - floor((extent->miny - grid->extent.miny) / unitheight + epsilon) + tolerance;
        limits[i].maxy = ceil((grid->extent.maxy - extent->miny) / unitheight - epsilon) + tolerance;
        //printf("%d: %d %d %d %d\n",i,limits[i].minx,limits[i].miny,limits[i].maxx,limits[i].maxy);
        break;
      case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
        break; /* not implemented */
    }
    // to avoid requesting out-of-range tiles
    if (limits[i].minx < 0) limits[i].minx = 0;
    if (limits[i].maxx > level->maxx) limits[i].maxx = level->maxx;
    if (limits[i].miny < 0) limits[i].miny = 0;
    if (limits[i].maxy > level->maxy) limits[i].maxy = level->maxy;

  }
}

double mapcache_grid_get_resolution(mapcache_extent *bbox, int sx, int sy)
{
  double rx =  mapcache_grid_get_horizontal_resolution(bbox,sx);
  double ry =  mapcache_grid_get_vertical_resolution(bbox,sy);
  return MAPCACHE_MAX(rx,ry);
}


double mapcache_grid_get_horizontal_resolution(mapcache_extent *bbox, int width)
{
  return (bbox->maxx - bbox->minx) / (double)width;
}

double mapcache_grid_get_vertical_resolution(mapcache_extent *bbox, int height)
{
  return (bbox->maxy - bbox->miny) / (double)height;
}

int mapcache_grid_get_level(mapcache_context *ctx, mapcache_grid *grid, double *resolution, int *level)
{
  double max_diff = *resolution / (double)MAPCACHE_MAX(grid->tile_sx, grid->tile_sy);
  int i;
  for(i=0; i<grid->nlevels; i++) {
    if(fabs(grid->levels[i]->resolution - *resolution) < max_diff) {
      *resolution = grid->levels[i]->resolution;
      *level = i;
      return MAPCACHE_SUCCESS;
    }
  }
  return MAPCACHE_FAILURE;
}

void mapcache_grid_get_closest_level(mapcache_context *ctx, mapcache_grid_link *grid_link, double resolution, int *level)
{
  double dst = fabs(grid_link->grid->levels[grid_link->minz]->resolution - resolution);
  int i;
  *level = 0;

  for(i=grid_link->minz + 1; i<grid_link->maxz; i++) {
    double curdst = fabs(grid_link->grid->levels[i]->resolution - resolution);
    if( curdst < dst) {
      dst = curdst;
      *level = i;
    }
  }
}

/*
 * update the tile by setting it's x,y,z value given a bbox.
 * will return MAPCACHE_TILESET_WRONG_RESOLUTION or MAPCACHE_TILESET_WRONG_EXTENT
 * if the bbox does not correspond to the tileset's configuration
 */
int mapcache_grid_get_cell(mapcache_context *ctx, mapcache_grid *grid, mapcache_extent *bbox,
                           int *x, int *y, int *z)
{
  double res = mapcache_grid_get_resolution(bbox,grid->tile_sx,grid->tile_sy);
  if(MAPCACHE_SUCCESS != mapcache_grid_get_level(ctx, grid, &res, z))
    return MAPCACHE_FAILURE;

  switch(grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      *x = (int)(((bbox->minx - grid->extent.minx) / (res * grid->tile_sx)) + 0.5);
      *y = (int)(((bbox->miny - grid->extent.miny) / (res * grid->tile_sy)) + 0.5);

      if((fabs(bbox->minx - (*x * res * grid->tile_sx) - grid->extent.minx ) / res > 1) ||
          (fabs(bbox->miny - (*y * res * grid->tile_sy) - grid->extent.miny ) / res > 1)) {
        return MAPCACHE_FAILURE;
      }
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      *x = (int)(((bbox->minx - grid->extent.minx) / (res * grid->tile_sx)) + 0.5);
      *y = (int)(((grid->extent.maxy - bbox->maxy) / (res * grid->tile_sy)) + 0.5);

      if((fabs(bbox->minx - (*x * res * grid->tile_sx) - grid->extent.minx ) / res > 1) ||
          (fabs(bbox->maxy - (grid->extent.maxy - (*y * res * grid->tile_sy)) ) / res > 1)) {
        return MAPCACHE_FAILURE;
      }
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      return MAPCACHE_FAILURE;
  }
  return MAPCACHE_SUCCESS;
}


void mapcache_grid_get_xy(mapcache_context *ctx, mapcache_grid *grid, double dx, double dy,
                          int z, int *x, int *y)
{
  double res;
#ifdef DEBUG
  if(z>=grid->nlevels) {
    ctx->set_error(ctx,500,"####BUG##### requesting invalid level");
    return;
  }
#endif
  res = grid->levels[z]->resolution;
  switch(grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      *x = (int)((dx - grid->extent.minx) / (res * grid->tile_sx));
      *y = (int)((dy - grid->extent.miny) / (res * grid->tile_sy));
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      *x = (int)((dx - grid->extent.minx) / (res * grid->tile_sx));
      *y = (int)((grid->extent.maxy - dy) / (res * grid->tile_sy));
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      ctx->set_error(ctx,500,"####BUG##### origin not implemented");
      return;
  }
}
/* vim: ts=2 sts=2 et sw=2
*/
