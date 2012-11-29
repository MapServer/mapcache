/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: KML superoverlay service
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

/** \addtogroup services */
/** @{ */


void _create_capabilities_kml(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  mapcache_request_get_capabilities_kml *request = (mapcache_request_get_capabilities_kml*)req;
  char *caps;
  const char *onlineresource = apr_table_get(cfg->metadata,"url");
  int i, j;
  if(!onlineresource) {
    onlineresource = url;
  }
  request->request.mime_type = apr_pstrdup(ctx->pool,"application/vnd.google-earth.kml+xml");

  assert(request->tile || (request->grid && request->tileset));

  /* if we have no specific tile, create a kml document referencing all the tiles of the first level in the grid*/
  if(!request->tile) {
    mapcache_extent extent = request->grid->restricted_extent?*(request->grid->restricted_extent):request->grid->grid->extent;
    caps = apr_psprintf(ctx->pool, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<kml xmlns=\"http://earth.google.com/kml/2.1\">\n"
                        "  <Document>\n"
                        "    <Region>\n"
                        "      <Lod>\n"
                        "        <minLodPixels>128</minLodPixels><maxLodPixels>512</maxLodPixels>\n"
                        "      </Lod>\n"
                        "      <LatLonAltBox>\n"
                        "        <north>%f</north><south>%f</south>\n"
                        "        <east>%f</east><west>%f</west>\n"
                        "      </LatLonAltBox>\n"
                        "    </Region>\n",
                        extent.maxy,extent.miny,extent.maxx,extent.minx);
    for(i=request->grid->grid_limits[0].minx; i<request->grid->grid_limits[0].maxx; i++) {
      for(j=request->grid->grid_limits[0].miny; j<request->grid->grid_limits[0].maxy; j++) {

        mapcache_tile *t = mapcache_tileset_tile_create(ctx->pool, request->tileset, request->grid);
        mapcache_extent bb;
        t->x = i;
        t->y = j;
        t->z = 0;
        mapcache_grid_get_extent(ctx, t->grid_link->grid,
                                 t->x, t->y, t->z, &bb);

        caps = apr_psprintf(ctx->pool, "%s"
                            "    <NetworkLink>\n"
                            "      <name>%d%d%d</name>\n"
                            "      <Region>\n"
                            "        <Lod>\n"
                            "          <minLodPixels>128</minLodPixels><maxLodPixels>-1</maxLodPixels>\n"
                            "        </Lod>\n"
                            "        <LatLonAltBox>\n"
                            "          <north>%f</north><south>%f</south>\n"
                            "          <east>%f</east><west>%f</west>\n"
                            "        </LatLonAltBox>\n"
                            "      </Region>\n"
                            "      <Link>\n"
                            "        <href>%s/kml/%s@%s/%d/%d/%d.kml</href>\n"
                            "        <viewRefreshMode>onRegion</viewRefreshMode>\n"
                            "      </Link>\n"
                            "    </NetworkLink>\n",
                            caps, t->x, t->y, t->z,
                            bb.maxy, bb.miny, bb.maxx, bb.minx,
                            onlineresource, request->tileset->name, request->grid->grid->name,
                            t->z, t->x, t->y);
      }
    }
    caps = apr_pstrcat(ctx->pool, caps, "  </Document>\n</kml>\n", NULL);
  } else {
    mapcache_extent bbox;

    mapcache_grid_get_extent(ctx, request->tile->grid_link->grid,
                             request->tile->x, request->tile->y, request->tile->z, &bbox);


    caps = apr_psprintf(ctx->pool, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<kml xmlns=\"http://earth.google.com/kml/2.1\">\n"
                        "  <Document>\n"
                        "    <Region>\n"
                        "      <Lod>\n"
                        "        <minLodPixels>128</minLodPixels><maxLodPixels>%d</maxLodPixels>\n"
                        "      </Lod>\n"
                        "      <LatLonAltBox>\n"
                        "        <north>%f</north><south>%f</south>\n"
                        "        <east>%f</east><west>%f</west>\n"
                        "      </LatLonAltBox>\n"
                        "    </Region>\n"
                        "    <GroundOverlay>\n"
                        "      <drawOrder>0</drawOrder>\n"
                        "      <Icon>\n"
                        "        <href>%s/tms/1.0.0/%s@%s/%d/%d/%d.%s</href>\n"
                        "      </Icon>\n"
                        "      <LatLonBox>\n"
                        "        <north>%f</north><south>%f</south>\n"
                        "        <east>%f</east><west>%f</west>\n"
                        "      </LatLonBox>\n"
                        "    </GroundOverlay>\n",
                        (request->tile->z == request->tile->grid_link->grid->nlevels - 1) ? -1 : 512,
                        bbox.maxy, bbox.miny, bbox.maxx, bbox.minx,
                        onlineresource, request->tile->tileset->name, request->tile->grid_link->grid->name,
                        request->tile->z, request->tile->x, request->tile->y,
                        (request->tile->tileset->format) ? request->tile->tileset->format->extension : "png",
                        bbox.maxy, bbox.miny, bbox.maxx, bbox.minx);

    if (request->tile->z < request->tile->grid_link->grid->nlevels - 1) {
      for (i = 0; i <= 1; i++) {
        for (j = 0; j <= 1; j++) {
          /* compute the addresses of the child tiles */
          mapcache_tile *t = mapcache_tileset_tile_create(ctx->pool, request->tile->tileset, request->tile->grid_link);
          mapcache_extent bb;
          t->x = (request->tile->x << 1) + i;
          t->y = (request->tile->y << 1) + j;
          t->z = request->tile->z + 1;
          mapcache_grid_get_extent(ctx, t->grid_link->grid,
                                   t->x, t->y, t->z, &bb);

          caps = apr_psprintf(ctx->pool, "%s"
                              "    <NetworkLink>\n"
                              "      <name>%d%d%d</name>\n"
                              "      <Region>\n"
                              "        <Lod>\n"
                              "          <minLodPixels>128</minLodPixels><maxLodPixels>-1</maxLodPixels>\n"
                              "        </Lod>\n"
                              "        <LatLonAltBox>\n"
                              "          <north>%f</north><south>%f</south>\n"
                              "          <east>%f</east><west>%f</west>\n"
                              "        </LatLonAltBox>\n"
                              "      </Region>\n"
                              "      <Link>\n"
                              "        <href>%s/kml/%s@%s/%d/%d/%d.kml</href>\n"
                              "        <viewRefreshMode>onRegion</viewRefreshMode>\n"
                              "      </Link>\n"
                              "    </NetworkLink>\n",
                              caps, t->x, t->y, t->z,
                              bb.maxy, bb.miny, bb.maxx, bb.minx,
                              onlineresource, request->tile->tileset->name, request->tile->grid_link->grid->name,
                              t->z, t->x, t->y);
        }
      }
    }

    caps = apr_pstrcat(ctx->pool, caps, "  </Document>\n</kml>\n", NULL);
  }
  request->request.capabilities = caps;


}

/**
 * \brief parse a KML request
 * \private \memberof mapcache_service_kml
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_kml_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int index = 0;
  char *last, *key, *endptr;
  mapcache_tileset *tileset = NULL;
  mapcache_grid_link *grid_link = NULL;
  char *pathinfo = NULL;
  int x=-1,y=-1,z=-1;

  if(cpathinfo) {
    pathinfo = apr_pstrdup(ctx->pool,cpathinfo);
    /* parse a path_info like /layer@grid/0/0/0.kml */
    for (key = apr_strtok(pathinfo, "/", &last); key != NULL;
         key = apr_strtok(NULL, "/", &last)) {
      if(!*key) continue; /* skip an empty string, could happen if the url contains // */
      switch(++index) {
        case 1: /* layer name */
          tileset = mapcache_configuration_get_tileset(config,key);
          if(!tileset) {
            /*tileset not found directly, test if it was given as "name@grid" notation*/
            char *tname = apr_pstrdup(ctx->pool,key);
            char *gname = tname;
            char*ext;
            int i;
            while(*gname) {
              if(*gname == '@') {
                *gname = '\0';
                gname++;
                break;
              }
              gname++;
            }
            if(!*gname) {
              ctx->set_error(ctx,404, "received kml request with invalid layer %s", key);
              return;
            }

            /* is this the first request, eg tileset@grid.kml? in that case reome the .kml
             from the grid name */
            ext = strstr(gname,".kml");
            if(ext) *ext = '\0';

            tileset = mapcache_configuration_get_tileset(config,tname);
            if(!tileset) {
              ctx->set_error(ctx,404, "received kml request with invalid layer %s", tname);
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
              ctx->set_error(ctx,404, "received kml request with invalid grid %s", gname);
              return;
            }

          } else {
            grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
          }
          break;
        case 2:
          z = (int)strtol(key,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,404, "received kml request %s with invalid z %s", pathinfo, key);
            return;
          }
          break;
        case 3:
          x = (int)strtol(key,&endptr,10);
          if(*endptr != 0) {
            ctx->set_error(ctx,404, "received kml request %s with invalid x %s", pathinfo, key);
            return;
          }
          break;
        case 4:
          y = (int)strtol(key,&endptr,10);
          if(*endptr != '.') {
            ctx->set_error(ctx,404, "received kml request %s with invalid y %s", pathinfo, key);
            return;
          }
          endptr++;
          if(strcmp(endptr,"kml")) {
            ctx->set_error(ctx,404, "received kml request with invalid extension %s", pathinfo, endptr);
            return;
          }
          break;
        default:
          ctx->set_error(ctx,404, "received kml request %s with invalid parameter %s", pathinfo, key);
          return;
      }
    }
  }
  if(index == 4) {
    mapcache_request_get_capabilities_kml *req = (mapcache_request_get_capabilities_kml*)apr_pcalloc(
          ctx->pool,sizeof(mapcache_request_get_capabilities_kml));
    req->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
    req->tile = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
    req->tile->x = x;
    req->tile->y = y;
    req->tile->z = z;
    mapcache_tileset_tile_validate(ctx,req->tile);
    GC_CHECK_ERROR(ctx);
    *request = (mapcache_request*)req;
    return;
  } else if(index==1) {
    mapcache_request_get_capabilities_kml *req = (mapcache_request_get_capabilities_kml*)apr_pcalloc(
          ctx->pool,sizeof(mapcache_request_get_capabilities_kml));
    req->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
    req->tile = NULL;
    req->tileset = tileset;
    req->grid = grid_link;
    *request = (mapcache_request*)req;
    return;
  } else {
    ctx->set_error(ctx,404, "received kml request %s with wrong number of arguments", pathinfo);
    return;
  }
}

mapcache_service* mapcache_service_kml_create(mapcache_context *ctx)
{
  mapcache_service_kml* service = (mapcache_service_kml*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_kml));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate kml service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool,"kml");
  service->service.name = apr_pstrdup(ctx->pool,"kml");
  service->service.type = MAPCACHE_SERVICE_KML;
  service->service.parse_request = _mapcache_service_kml_parse_request;
  service->service.create_capabilities_response = _create_capabilities_kml;
  return (mapcache_service*)service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
