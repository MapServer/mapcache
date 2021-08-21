/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Virtualearth quadkey service
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
#include <math.h>
#include "mapcache_services.h"

/** \addtogroup services */

/** @{ */


void _create_capabilities_ve(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  ctx->set_error(ctx, 501, "ve service does not support capapbilities");
}

/**
 * \brief parse a VE request
 * \private \memberof mapcache_service_ve
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_ve_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int i,x,y,z;
  const char *layer, *quadkey;
  mapcache_tileset *tileset = NULL;
  mapcache_grid_link *grid_link = NULL;
  mapcache_tile *tile;
  mapcache_request_get_tile *req;

  layer = apr_table_get(params, "layer");
  if (layer) {
    /*tileset not found directly, test if it was given as "name@grid" notation*/
    char *tname = apr_pstrdup(ctx->pool, layer);
    char *gname = tname;
    while (*gname) {
      if (*gname == '@') {
        *gname = '\0';
        gname++;
        break;
      }
      gname++;
    }
    if (!gname) {
      ctx->set_error(ctx, 404, "received ve request with invalid layer %s", layer);
      return;
    }
    tileset = mapcache_configuration_get_tileset(config, tname);
    if (!tileset) {
      ctx->set_error(ctx, 404, "received ve request with invalid layer %s", tname);
      return;
    }
    for (i = 0; i < tileset->grid_links->nelts; i++) {
      mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links, i, mapcache_grid_link*);
      if (!strcmp(sgrid->grid->name, gname)) {
        grid_link = sgrid;
        break;
      }
    }
    if (!grid_link) {
      ctx->set_error(ctx, 404, "received ve request with invalid grid %s", gname);
      return;
    }
  } else {
    ctx->set_error(ctx, 400, "received ve request with no layer");
    return;
  }

  quadkey = apr_table_get(params, "tile");
  tile = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
  if (quadkey) {
    mapcache_util_quadkey_decode(ctx, quadkey, &x, &y, &z);
    GC_CHECK_ERROR(ctx);
    if (z < 1 || z >= grid_link->grid->nlevels) {
      ctx->set_error(ctx, 404, "received ve request with invalid z level %d\n", z);
      return;
    }
  } else {
    ctx->set_error(ctx, 400, "received ve request with no tile quadkey");
    return;
  }


  req = (mapcache_request_get_tile*) apr_pcalloc(ctx->pool, sizeof (mapcache_request_get_tile));
  ((mapcache_request*)req)->type = MAPCACHE_REQUEST_GET_TILE;
  req->ntiles = 1;
  req->tiles = (mapcache_tile**) apr_pcalloc(ctx->pool, sizeof (mapcache_tile*));
  req->tiles[0] = tile;
  tile->z = z;
  switch (grid_link->grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      req->tiles[0]->x = x;
      req->tiles[0]->y = grid_link->grid->levels[z]->maxy - y - 1;
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      req->tiles[0]->x = x;
      req->tiles[0]->y = y;
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
      req->tiles[0]->x = grid_link->grid->levels[z]->maxx - x - 1;
      req->tiles[0]->y = y;
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      req->tiles[0]->x = grid_link->grid->levels[z]->maxx - x - 1;
      req->tiles[0]->y = grid_link->grid->levels[z]->maxy - y - 1;
      break;
  }
  mapcache_tileset_tile_validate(ctx, req->tiles[0]);
  GC_CHECK_ERROR(ctx);
  *request = (mapcache_request*) req;
  return;
}

mapcache_service* mapcache_service_ve_create(mapcache_context *ctx)
{
  mapcache_service_ve* service = (mapcache_service_ve*) apr_pcalloc(ctx->pool, sizeof (mapcache_service_ve));
  if (!service) {
    ctx->set_error(ctx, 500, "failed to allocate ve service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool, "ve");
  service->service.name = apr_pstrdup(ctx->pool, "ve");
  service->service.type = MAPCACHE_SERVICE_VE;
  service->service.parse_request = _mapcache_service_ve_parse_request;
  service->service.create_capabilities_response = _create_capabilities_ve;
  return (mapcache_service*) service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
