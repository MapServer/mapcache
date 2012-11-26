/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: TMS, XYZ and Gmaps service
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
#include <ezxml.h>
/** \addtogroup services */
/** @{ */


void _create_capabilities_tms(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  ezxml_t caps;
  char *tmpcaps;
  const char *onlineresource;
  mapcache_request_get_capabilities_tms *request = (mapcache_request_get_capabilities_tms*)req;
#ifdef DEBUG
  if(request->request.request.type != MAPCACHE_REQUEST_GET_CAPABILITIES) {
    ctx->set_error(ctx,500,"wrong tms capabilities request");
    return;
  }
#endif

  onlineresource = apr_table_get(cfg->metadata,"url");
  if(!onlineresource) {
    onlineresource = url;
  }

  request->request.mime_type = apr_pstrdup(ctx->pool,"text/xml");
  if(!request->version) {
    ezxml_t TileMapService;
    char* serviceurl;
    caps = ezxml_new("Services");
    TileMapService = ezxml_add_child(caps,"TileMapService",0);
    ezxml_set_attr(TileMapService,"version","1.0");
    serviceurl = apr_pstrcat(ctx->pool,onlineresource,"tms/1.0.0/",NULL);
    ezxml_set_attr(TileMapService,"href",serviceurl);
  } else {
    if(!request->tileset) {
      apr_hash_index_t *tileindex_index;
      ezxml_t tilemaps;
      caps = ezxml_new("TileMapService");
      ezxml_set_attr(caps,"version",request->version);
      tileindex_index = apr_hash_first(ctx->pool,cfg->tilesets);
      tilemaps = ezxml_add_child(caps,"TileMaps",0);
      while(tileindex_index) {
        mapcache_tileset *tileset;
        int j;
        const void *key;
        apr_ssize_t keylen;
        const char *title;
        apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);
        title = apr_table_get(tileset->metadata,"title");
        if(!title) {
          title = "no title set, add some in metadata";
        }
        for(j=0; j<tileset->grid_links->nelts; j++) {
          ezxml_t tilemap;
          char *href;
          mapcache_grid *grid = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*)->grid;
          /* TMS only supports tilesets with bottom-left origin*/
          if(grid->origin == MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT) {
            const char *profile = apr_table_get(grid->metadata,"profile");
            if(!profile) profile = "none";
            tilemap = ezxml_add_child(tilemaps,"TileMap",0);
            ezxml_set_attr(tilemap,"title",title);
            ezxml_set_attr(tilemap,"srs",grid->srs);
            if(profile)
              ezxml_set_attr(tilemap,"profile",profile);
            href = apr_pstrcat(ctx->pool,onlineresource,"tms/1.0.0/",tileset->name,"@",grid->name,NULL);
            ezxml_set_attr(tilemap,"href",href);
          }
        }
        tileindex_index = apr_hash_next(tileindex_index);
      }
    } else {
      const char *title;
      const char *abstract;
      ezxml_t origin;
      ezxml_t bbox;
      ezxml_t tileformat;
      ezxml_t tilesets;
      mapcache_tileset *tileset = request->tileset;
      mapcache_grid_link *grid_link = request->grid_link;
      mapcache_grid *grid = grid_link->grid;
      int i;
      mapcache_extent *extent = (request->grid_link->restricted_extent)?request->grid_link->restricted_extent:&request->grid_link->grid->extent;
      title = apr_table_get(tileset->metadata,"title");
      if(!title) {
        title = "no title set, add some in metadata";
      }
      abstract = apr_table_get(tileset->metadata,"abstract");
      if(!abstract) {
        abstract = "no abstract set, add some in metadata";
      }
      caps = ezxml_new("TileMap");
      ezxml_set_attr(caps,"version",request->version);
      ezxml_set_attr(caps,"tilemapservice",
                     apr_pstrcat(ctx->pool,onlineresource,"tms/",request->version,"/",NULL));

      ezxml_set_txt(ezxml_add_child(caps,"Title",0),title);
      ezxml_set_txt(ezxml_add_child(caps,"Abstract",0),abstract);
      ezxml_set_txt(ezxml_add_child(caps,"SRS",0),grid->srs);

      bbox = ezxml_add_child(caps,"BoundingBox",0);
      ezxml_set_attr(bbox,"minx",apr_psprintf(ctx->pool,"%f",extent->minx));
      ezxml_set_attr(bbox,"miny",apr_psprintf(ctx->pool,"%f",extent->miny));
      ezxml_set_attr(bbox,"maxx",apr_psprintf(ctx->pool,"%f",extent->maxx));
      ezxml_set_attr(bbox,"maxy",apr_psprintf(ctx->pool,"%f",extent->maxy));

      origin = ezxml_add_child(caps,"Origin",0);
      ezxml_set_attr(origin,"x",apr_psprintf(ctx->pool,"%f",grid->extent.minx));
      ezxml_set_attr(origin,"y",apr_psprintf(ctx->pool,"%f",grid->extent.miny));

      tileformat = ezxml_add_child(caps,"TileFormat",0);
      ezxml_set_attr(tileformat,"width",apr_psprintf(ctx->pool,"%d",grid->tile_sx));
      ezxml_set_attr(tileformat,"height",apr_psprintf(ctx->pool,"%d",grid->tile_sy));
      if(tileset->format && tileset->format->mime_type) {
        ezxml_set_attr(tileformat,"mime-type",tileset->format->mime_type);
      } else {
        ezxml_set_attr(tileformat,"mime-type","image/unknown");
      }
      if(tileset->format) {
        ezxml_set_attr(tileformat,"extension",tileset->format->extension);
      } else {
        ezxml_set_attr(tileformat,"extension","xxx");
      }

      tilesets = ezxml_add_child(caps,"TileSets",0);
      for(i=grid_link->minz; i<grid_link->maxz; i++) {
        ezxml_t xmltileset = ezxml_add_child(tilesets,"TileSet",0);
        char *order = apr_psprintf(ctx->pool,"%d",i);
        ezxml_set_attr(xmltileset,"href",
                       apr_pstrcat(ctx->pool,onlineresource,"tms/",request->version,"/",
                                   tileset->name,"@",grid->name,
                                   "/",order,NULL));
        ezxml_set_attr(xmltileset,"units-per-pixel",apr_psprintf(ctx->pool,"%.20f",grid->levels[i]->resolution));
        ezxml_set_attr(xmltileset,"order",order);
      }
    }
  }
  tmpcaps = ezxml_toxml(caps);
  ezxml_free(caps);
  request->request.capabilities = apr_pstrdup(ctx->pool,tmpcaps);
  free(tmpcaps);


}

/**
 * \brief parse a TMS request
 * \private \memberof mapcache_service_tms
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_tms_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int index = 0;
  char *last, *key, *endptr;
  char *sTileset = NULL;
  mapcache_tileset *tileset = NULL;
  mapcache_grid_link *grid_link = NULL;
  char *pathinfo = NULL;
  int x=-1,y=-1,z=-1;

  if(this->type == MAPCACHE_SERVICE_GMAPS) {
    index++;
    /* skip the version part of the url */
  }
  if(cpathinfo) {
    pathinfo = apr_pstrdup(ctx->pool,cpathinfo);
    /* parse a path_info like /1.0.0/global_mosaic/0/0/0.jpg */
    for (key = apr_strtok(pathinfo, "/", &last); key != NULL;
         key = apr_strtok(NULL, "/", &last)) {
      if(!*key) continue; /* skip an empty string, could happen if the url contains // */
      switch(++index) {
        case 1: /* version */
          if(strcmp("1.0.0",key)) {
            ctx->set_error(ctx,404, "received tms request with invalid version %s", key);
            return;
          }
          break;
        case 2: /* layer name */
          sTileset = apr_pstrdup(ctx->pool,key);
          break;
        case 3:
          z = (int)strtol(key,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,404, "failed to parse z");
            return;
          }
          break;
        case 4:
          x = (int)strtol(key,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,404, "failed to parse x");
            return;
          }
          break;
        case 5:
          y = (int)strtol(key,&endptr,10);
          if(*endptr != '.') {
            ctx->set_error(ctx,404, "failed to parse y");
            return;
          }
          break;
        default:
          ctx->set_error(ctx,404, "received tms request %s with invalid parameter %s", pathinfo, key);
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
          ctx->set_error(ctx,404, "received tms request with invalid layer %s", key);
          return;
        }
        tileset = mapcache_configuration_get_tileset(config,tname);
        if(!tileset) {
          ctx->set_error(ctx,404, "received tms request with invalid layer %s", tname);
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
          ctx->set_error(ctx,404, "received tms request with invalid grid %s", gname);
          return;
        }

      } else {
        grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
      }
      if(!gridname) {
        gridname = grid_link->grid->name;
      } else {
        if(strcmp(gridname,grid_link->grid->name)) {
          ctx->set_error(ctx,400,"received tms request with conflicting grids %s and %s",
                         gridname,grid_link->grid->name);
          return;
        }
      }
      if(((mapcache_service_tms*)this)->reverse_y) {
        y = grid_link->grid->levels[z]->maxy - y - 1;
      }
      req->tiles[req->ntiles] = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
      switch(grid_link->grid->origin) {
        case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = y;
          break;
        case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
          req->tiles[req->ntiles]->x = x;
          req->tiles[req->ntiles]->y = grid_link->grid->levels[z]->maxy - y - 1;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
          req->tiles[req->ntiles]->x = grid_link->grid->levels[z]->maxx - x - 1;
          req->tiles[req->ntiles]->y = y;
          break;
        case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
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
  } else if(index<3 && this->type == MAPCACHE_SERVICE_TMS) {
    mapcache_request_get_capabilities_tms *req = (mapcache_request_get_capabilities_tms*)apr_pcalloc(
          ctx->pool,sizeof(mapcache_request_get_capabilities_tms));
    req->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
    if(index == 2) {
      tileset = mapcache_configuration_get_tileset(config,sTileset);
      if(!tileset) {
        /*tileset not found directly, test if it was given as "name@grid" notation*/
        char *tname = apr_pstrdup(ctx->pool,sTileset);
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
          ctx->set_error(ctx,404, "received tms request with invalid layer %s", key);
          return;
        }
        tileset = mapcache_configuration_get_tileset(config,tname);
        if(!tileset) {
          ctx->set_error(ctx,404, "received tms request with invalid layer %s", tname);
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
          ctx->set_error(ctx,404, "received tms request with invalid grid %s", gname);
          return;
        }
      } else {
        grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
      }
      req->tileset = tileset;
      req->grid_link = grid_link;
    }
    if(index >= 1) {
      req->version = apr_pstrdup(ctx->pool,"1.0.0");
    }
    *request = (mapcache_request*)req;
    return;
  } else {
    ctx->set_error(ctx,404, "received request with wrong number of arguments", pathinfo);
    return;
  }
}

mapcache_service* mapcache_service_tms_create(mapcache_context *ctx)
{
  mapcache_service_tms* service = (mapcache_service_tms*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_tms));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate tms service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool,"tms");
  service->service.name = apr_pstrdup(ctx->pool,"tms");
  service->service.type = MAPCACHE_SERVICE_TMS;
  service->reverse_y = 0;
  service->service.parse_request = _mapcache_service_tms_parse_request;
  service->service.create_capabilities_response = _create_capabilities_tms;
  return (mapcache_service*)service;
}

void _create_capabilities_gmaps(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  ctx->set_error(ctx,404,"gmaps service does not support capabilities");
}

mapcache_service* mapcache_service_gmaps_create(mapcache_context *ctx)
{
  mapcache_service_tms* service = (mapcache_service_tms*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_tms));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate gmaps service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool,"gmaps");
  service->service.name = apr_pstrdup(ctx->pool,"gmaps");
  service->reverse_y = 1;
  service->service.type = MAPCACHE_SERVICE_GMAPS;
  service->service.parse_request = _mapcache_service_tms_parse_request;
  service->service.create_capabilities_response = _create_capabilities_gmaps;
  return (mapcache_service*)service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
