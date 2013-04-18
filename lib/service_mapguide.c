/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Mapguide HTTPTile service
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2013 Regents of the University of Minnesota.
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

/** \addtogroup services */

/** @{ */


void _create_capabilities_mg(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  ctx->set_error(ctx, 501, "mapguide service does not support capapbilities");
}

/**
 * \brief parse a Mapguide HTTPTile request
 * \private \memberof mapcache_service_mg
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_mg_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int index = 0;
  char *last, *key, *endptr;
  char *sTileset = NULL;
  mapcache_tileset *tileset = NULL;
  mapcache_grid_link *grid_link = NULL;
  char *pathinfo = NULL;
  int x=-1,y=-1,z=-1,ygroup=-1,xgroup=1;

  if(cpathinfo) {
    pathinfo = apr_pstrdup(ctx->pool,cpathinfo);
    /* parse a path_info like /1.0.0/global_mosaic/0/0/0.jpg */
    for (key = apr_strtok(pathinfo, "/", &last); key != NULL;
         key = apr_strtok(NULL, "/", &last)) {
      if(!*key) continue; /* skip an empty string, could happen if the url contains // */
      switch(++index) {
        case 1: /* S[level] */
          if(*key!='S') {
            ctx->set_error(ctx,400, "received mapguide request with invalid level %s", key);
            return;
          }
          z = (int)strtol(key+1,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,400, "failed to parse S level");
            return;
          }
          break;
        case 2: /* layer name */
          sTileset = apr_pstrdup(ctx->pool,key);
          break;
        case 3: /*rowgroup*/
          if(*key!='R') {
            ctx->set_error(ctx,400, "received mapguide request with invalid rowgroup %s", key);
            return;
          }
          ygroup = (int)strtol(key+1,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,400, "failed to parse rowgroup");
            return;
          }
          break;
        case 4: /*colgroup*/
          if(*key!='C') {
            ctx->set_error(ctx,400, "received mapguide request with invalid colgroup %s", key);
            return;
          }
          xgroup = (int)strtol(key+1,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,404, "failed to parse colgroup");
            return;
          }
          break;
        case 5:
          y = (int)strtol(key,&endptr,10);
          if(*endptr != '_') {
            ctx->set_error(ctx,404, "failed to parse y");
            return;
          }
          key = endptr+1;
          x = (int)strtol(key,&endptr,10);
          if(*endptr != '.') {
            ctx->set_error(ctx,404, "failed to parse x");
            return;
          }
          x += xgroup; 
          y += ygroup; 
          break;
        default:
          ctx->set_error(ctx,404, "received mapguide request %s with invalid parameter %s", pathinfo, key);
          return;
      }
    }
  }
  if(index == 5) {
    char *gridname;
    mapcache_request_get_tile *req = (mapcache_request_get_tile*)apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_tile));
    req->request.type = MAPCACHE_REQUEST_GET_TILE;
    gridname = sTileset;  /*hijack the char* pointer while counting the number of commas */
    while(*gridname) {
      if(*gridname == ';') req->ntiles++;
      gridname++;
    }
    req->tiles = (mapcache_tile**)apr_pcalloc(ctx->pool,(req->ntiles+1) * sizeof(mapcache_tile*));

    /* reset the hijacked variables back to default value */
    gridname = NULL;
    req->ntiles = 0;

    for (key = apr_strtok(sTileset, ";", &last); key != NULL;
         key = apr_strtok(NULL, ";", &last)) {
      tileset = mapcache_configuration_get_tileset(config,key);
      if(!tileset) {
        /*tileset not found directly, test if it was given as "name@grid" notation*/
        char *tname = apr_pstrdup(ctx->pool,key);
        char *gname = tname;
        int i;
        while(*gname) {
          if(*gname == '@') {
            *gname = '\0';
            gname++;
            break;
          }
          gname++;
        }
        if(!gname) {
          ctx->set_error(ctx,404, "received mapguide request with invalid layer %s", key);
          return;
        }
        tileset = mapcache_configuration_get_tileset(config,tname);
        if(!tileset) {
          ctx->set_error(ctx,404, "received mapguide request with invalid layer %s", tname);
          return;
        }
        for(i=0; i<tileset->grid_links->nelts; i++) {
          mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
          if(!strcmp(sgrid->grid->name,gname)) {
            grid_link = sgrid;
            break;
          }
        }
        if(!grid_link) {
          ctx->set_error(ctx,404, "received mapguide request with invalid grid %s", gname);
          return;
        }

      } else {
        grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
      }
      if(!gridname) {
        gridname = grid_link->grid->name;
        z = grid_link->maxz - z - 1;
        if(z<0 || z>=grid_link->maxz) {
          ctx->set_error(ctx,404,"invalid z level");
          return;
        }
      } else {
        if(strcmp(gridname,grid_link->grid->name)) {
          ctx->set_error(ctx,400,"received mapguide request with conflicting grids %s and %s",
                         gridname,grid_link->grid->name);
          return;
        }
      }
      req->tiles[req->ntiles] = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
      switch(grid_link->grid->origin) {
        case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = y;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = grid_link->grid->levels[z]->maxy - y - 1;
          break;
        case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
          req->tiles[req->ntiles]->x = grid_link->grid->levels[z]->maxx - x - 1;
          req->tiles[req->ntiles]->y = y;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
          req->tiles[req->ntiles]->x = grid_link->grid->levels[z]->maxx - x - 1;
          req->tiles[req->ntiles]->y = grid_link->grid->levels[z]->maxy - y - 1;
          break;
      }
      req->tiles[req->ntiles]->z = z;
      mapcache_tileset_tile_validate(ctx,req->tiles[req->ntiles]);
      req->ntiles++;
      GC_CHECK_ERROR(ctx);
    }
    *request = (mapcache_request*)req;
    return;
  } else {
    ctx->set_error(ctx,404, "received request with wrong number of arguments", pathinfo);
    return;
  }
}

void _mapcache_service_mg_configuration_xml_parse(mapcache_context *ctx, ezxml_t node, mapcache_service *gservice, mapcache_cfg *cfg) {
  const char* attr = ezxml_attr(node,"rows_per_folder");
  mapcache_service_mapguide *service = (mapcache_service_mapguide*)gservice;
  char *endptr;
  if(attr && *attr) {
    service->rows_per_folder = (int)strtol(attr,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400, "failed to parse rows_per_folder attribute");
      return;
    }
  }
  attr = ezxml_attr(node,"cols_per_folder");
  if(attr && *attr) {
    service->cols_per_folder = (int)strtol(attr,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400, "failed to parse cols_per_folder attribute");
      return;
    }
  }
  
}

mapcache_service* mapcache_service_mapguide_create(mapcache_context *ctx)
{
  mapcache_service_mapguide* service = (mapcache_service_mapguide*) apr_pcalloc(ctx->pool, sizeof (mapcache_service_mapguide));
  if (!service) {
    ctx->set_error(ctx, 500, "failed to allocate mapguide service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool, "mg");
  service->service.name = apr_pstrdup(ctx->pool, "mapguide");
  service->service.type = MAPCACHE_SERVICE_MAPGUIDE;
  service->service.parse_request = _mapcache_service_mg_parse_request;
  service->service.configuration_parse_xml = _mapcache_service_mg_configuration_xml_parse;
  service->service.create_capabilities_response = _create_capabilities_mg;
  service->rows_per_folder = 30;
  service->cols_per_folder = 30;
  return (mapcache_service*) service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
