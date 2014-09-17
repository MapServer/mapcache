/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: WMS and OGC forwarding service
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
#include "ezxml.h"

/** \addtogroup services */
/** @{ */

void _create_capabilities_wms(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *guessed_url, char *path_info, mapcache_cfg *cfg)
{
  ezxml_t caps, tmpxml;
  const char *title;
  const char *url;
  ezxml_t capxml;
  ezxml_t reqxml;
  ezxml_t vendorxml;
  ezxml_t toplayer;
  apr_hash_index_t *grid_index;
  apr_hash_index_t *tileindex_index;
  char *tmpcaps;
  static char *capheader;
  mapcache_request_get_capabilities_wms *request = (mapcache_request_get_capabilities_wms*)req;
#ifdef DEBUG
  if(request->request.request.type != MAPCACHE_REQUEST_GET_CAPABILITIES) {
    ctx->set_error(ctx,400,"wrong wms capabilities request");
    return;
  }
#endif

  url = apr_table_get(cfg->metadata,"url");
  if(!url) {
    url = guessed_url;
  }

  url = apr_pstrcat(ctx->pool,url,req->request.service->url_prefix,"?",NULL);
  caps = ezxml_new("WMT_MS_Capabilities");
  ezxml_set_attr(caps,"version","1.1.1");
  /*
            "<Service>\n"
              "<Name>OGC:WMS</Name>\n"
              "<Title>%s</Title>\n"
              "<OnlineResource xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:href=\"%s/wms?\"/>\n"
            "</Service>\n"
  */
  tmpxml = ezxml_add_child(caps,"Service",0);
  ezxml_set_txt(ezxml_add_child(tmpxml,"Name",0),"OGC:WMS");
  title = apr_table_get(cfg->metadata,"title");
  if(!title) {
    title = "no title set, add some in metadata";
  }
  ezxml_set_txt(ezxml_add_child(tmpxml,"Title",0),title);
  tmpxml = ezxml_add_child(tmpxml,"OnlineResource",0);
  ezxml_set_attr(tmpxml,"xmlns:xlink","http://www.w3.org/1999/xlink");
  ezxml_set_attr(tmpxml,"xlink:href",url);
  /*

     "<Capability>\n"
     "<Request>\n"
  */
  capxml = ezxml_add_child(caps,"Capability",0);
  reqxml = ezxml_add_child(capxml,"Request",0);
  /*
     "<GetCapabilities>\n"
     " <Format>application/vnd.ogc.wms_xml</Format>\n"
     " <DCPType>\n"
     "  <HTTP>\n"
     "   <Get><OnlineResource xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:href=\"%s/wms?\"/></Get>\n"
     "  </HTTP>\n"
     " </DCPType>\n"
     "</GetCapabilities>\n"
     */
  tmpxml = ezxml_add_child(reqxml,"GetCapabilities",0);
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"application/vnd.ogc.wms_xml");
  tmpxml = ezxml_add_child(tmpxml,"DCPType",0);
  tmpxml = ezxml_add_child(tmpxml,"HTTP",0);
  tmpxml = ezxml_add_child(tmpxml,"Get",0);
  tmpxml = ezxml_add_child(tmpxml,"OnlineResource",0);
  ezxml_set_attr(tmpxml,"xmlns:xlink","http://www.w3.org/1999/xlink");
  ezxml_set_attr(tmpxml,"xlink:href",url);

  /*
                "<GetMap>\n"
                  "<Format>image/png</Format>\n"
                  "<Format>image/jpeg</Format>\n"
                  "<DCPType>\n"
                    "<HTTP>\n"
                      "<Get><OnlineResource xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:href=\"%s/wms?\"/></Get>\n"
                    "</HTTP>\n"
                  "</DCPType>\n"
                "</GetMap>\n"
  */
  tmpxml = ezxml_add_child(reqxml,"GetMap",0);
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"image/png");
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"image/jpeg");
  tmpxml = ezxml_add_child(tmpxml,"DCPType",0);
  tmpxml = ezxml_add_child(tmpxml,"HTTP",0);
  tmpxml = ezxml_add_child(tmpxml,"Get",0);
  tmpxml = ezxml_add_child(tmpxml,"OnlineResource",0);
  ezxml_set_attr(tmpxml,"xmlns:xlink","http://www.w3.org/1999/xlink");
  ezxml_set_attr(tmpxml,"xlink:href",url);


  /*
                "<GetFeatureInfo>\n"
                  "<Format>text/plain</Format>\n"
                  "<Format>application/vnd.ogc.gml</Format>\n"
                  "<DCPType>\n"
                    "<HTTP>\n"
                      "<Get>\n"
                        "<OnlineResource xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:type=\"simple\" xlink:href=\"%s/wms?\" />\n"
                      "</Get>\n"
                    "</HTTP>\n"
                  "</DCPType>\n"
                "</GetFeatureInfo>\n"
  */
  tmpxml = ezxml_add_child(reqxml,"GetFeatureInfo",0);
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"text/plain");
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"application/vnd.ogc.gml");
  tmpxml = ezxml_add_child(tmpxml,"DCPType",0);
  tmpxml = ezxml_add_child(tmpxml,"HTTP",0);
  tmpxml = ezxml_add_child(tmpxml,"Get",0);
  tmpxml = ezxml_add_child(tmpxml,"OnlineResource",0);
  ezxml_set_attr(tmpxml,"xmlns:xlink","http://www.w3.org/1999/xlink");
  ezxml_set_attr(tmpxml,"xlink:href",url);

  /*
              "<Exception>\n"
                "<Format>text/plain</Format>\n"
              "</Exception>\n"
  */

  tmpxml = ezxml_add_child(capxml,"Exceptions",0);
  ezxml_set_txt(ezxml_add_child(tmpxml,"Format",0),"text/plain");

  vendorxml = ezxml_add_child(capxml,"VendorSpecificCapabilities",0);
  toplayer = ezxml_add_child(capxml,"Layer",0);
  tmpxml = ezxml_add_child(toplayer,"Title",0);
  ezxml_set_txt(tmpxml,title);

  /*
   * announce all layer srs's in the root layer. This part of the wms spec we
   * cannot respect with a caching solution, as each tileset can only be served
   * under a specified number of projections.
   *
   * TODO: check for duplicates in gris srs
   */
  grid_index = apr_hash_first(ctx->pool,cfg->grids);
  while(grid_index) {
    const void *key;
    apr_ssize_t keylen;
    mapcache_grid *grid = NULL;
    apr_hash_this(grid_index,&key,&keylen,(void**)&grid);
    ezxml_set_txt(ezxml_add_child(toplayer,"SRS",0),grid->srs);
    grid_index = apr_hash_next(grid_index);
  }


  tileindex_index = apr_hash_first(ctx->pool,cfg->tilesets);

  while(tileindex_index) {
    mapcache_tileset *tileset;
    ezxml_t layerxml;
    ezxml_t tsxml;
    const void *key;
    apr_ssize_t keylen;
    const char *title;
    const char *abstract;
    int i;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    layerxml = ezxml_add_child(toplayer,"Layer",0);
    ezxml_set_attr(layerxml, "cascaded", "1");
    ezxml_set_attr(layerxml, "queryable", (tileset->source && tileset->source->info_formats)?"1":"0");

    ezxml_set_txt(ezxml_add_child(layerxml,"Name",0),tileset->name);
    tsxml = ezxml_add_child(vendorxml, "TileSet",0);

    /*optional layer title*/
    title = apr_table_get(tileset->metadata,"title");
    if(title) {
      ezxml_set_txt(ezxml_add_child(layerxml,"Title",0),title);
    } else {
      ezxml_set_txt(ezxml_add_child(layerxml,"Title",0),tileset->name);
    }

    /*optional layer abstract*/
    abstract = apr_table_get(tileset->metadata,"abstract");
    if(abstract) {
      ezxml_set_txt(ezxml_add_child(layerxml,"Abstract",0),abstract);
    }

    if(tileset->wgs84bbox.minx != tileset->wgs84bbox.maxx) {
      ezxml_t wgsxml = ezxml_add_child(layerxml,"LatLonBoundingBox",0);
      ezxml_set_attr(wgsxml,"minx",apr_psprintf(ctx->pool,"%f",tileset->wgs84bbox.minx));
      ezxml_set_attr(wgsxml,"miny",apr_psprintf(ctx->pool,"%f",tileset->wgs84bbox.miny));
      ezxml_set_attr(wgsxml,"maxx",apr_psprintf(ctx->pool,"%f",tileset->wgs84bbox.maxx));
      ezxml_set_attr(wgsxml,"maxy",apr_psprintf(ctx->pool,"%f",tileset->wgs84bbox.maxy));
    }

    if(tileset->dimensions) {
      for(i=0; i<tileset->dimensions->nelts; i++) {
        const char **value;
        char *dimval;
        mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
        ezxml_t dimxml = ezxml_add_child(layerxml,"Dimension",0);
        ezxml_set_attr(dimxml,"name",dimension->name);
        ezxml_set_attr(dimxml,"default",dimension->default_value);

        if(dimension->unit) {
          ezxml_set_attr(dimxml,"units",dimension->unit);
        }
        value = dimension->print_ogc_formatted_values(ctx,dimension);
        dimval = apr_pstrdup(ctx->pool,*value);
        value++;
        while(*value) {
          dimval = apr_pstrcat(ctx->pool,dimval,",",*value,NULL);
          value++;
        }
        ezxml_set_txt(dimxml,dimval);
      }
    }


    for(i=0; i<tileset->grid_links->nelts; i++) {
      int j;
      ezxml_t bboxxml;
      mapcache_grid_link *gridlink = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
      mapcache_grid *grid = gridlink->grid;
      mapcache_extent *extent = &(grid->extent);
      if(gridlink->restricted_extent)
        extent = gridlink->restricted_extent;
      bboxxml = ezxml_add_child(layerxml,"BoundingBox",0);
      ezxml_set_attr(bboxxml,"SRS", grid->srs);
      ezxml_set_attr(bboxxml,"minx",apr_psprintf(ctx->pool,"%f",extent->minx));
      ezxml_set_attr(bboxxml,"miny",apr_psprintf(ctx->pool,"%f",extent->miny));
      ezxml_set_attr(bboxxml,"maxx",apr_psprintf(ctx->pool,"%f",extent->maxx));
      ezxml_set_attr(bboxxml,"maxy",apr_psprintf(ctx->pool,"%f",extent->maxy));
      ezxml_set_txt(ezxml_add_child(layerxml,"SRS",0),grid->srs);

      for(j=0; j<gridlink->grid->srs_aliases->nelts; j++) {
        ezxml_set_txt(ezxml_add_child(layerxml,"SRS",0),APR_ARRAY_IDX(gridlink->grid->srs_aliases,j,char*));
      }


      if(i==0) {
        char *resolutions;
        int i;
        /*wms-c only supports one grid per layer, so we use the first of the tileset's grids */
        ezxml_set_txt(ezxml_add_child(tsxml,"SRS",0),grid->srs);
        tmpxml = ezxml_add_child(tsxml,"BoundingBox",0);
        ezxml_set_attr(tmpxml,"SRS",grid->srs);
        ezxml_set_attr(tmpxml,"minx",apr_psprintf(ctx->pool,"%f",grid->extent.minx));
        ezxml_set_attr(tmpxml,"miny",apr_psprintf(ctx->pool,"%f",grid->extent.miny));
        ezxml_set_attr(tmpxml,"maxx",apr_psprintf(ctx->pool,"%f",grid->extent.maxx));
        ezxml_set_attr(tmpxml,"maxy",apr_psprintf(ctx->pool,"%f",grid->extent.maxy));

        resolutions="";

        for(i=gridlink->minz; i<gridlink->maxz; i++) {
          resolutions = apr_psprintf(ctx->pool,"%s%.20f ",resolutions,grid->levels[i]->resolution);
        }
        ezxml_set_txt(ezxml_add_child(tsxml,"Resolutions",0),resolutions);
        ezxml_set_txt(ezxml_add_child(tsxml,"Width",0),apr_psprintf(ctx->pool,"%d",grid->tile_sx));
        ezxml_set_txt(ezxml_add_child(tsxml,"Height",0),apr_psprintf(ctx->pool,"%d", grid->tile_sy));
      }
    }
    if(tileset->format && tileset->format->mime_type) {
      ezxml_set_txt(ezxml_add_child(tsxml,"Format",0),tileset->format->mime_type);
    } else {
      ezxml_set_txt(ezxml_add_child(tsxml,"Format",0),"image/unknown");
    }
    ezxml_set_txt(ezxml_add_child(tsxml,"Layers",0),tileset->name);
    ezxml_set_txt(ezxml_add_child(tsxml,"Styles",0),"");
    tileindex_index = apr_hash_next(tileindex_index);
  }


  tmpcaps = ezxml_toxml(caps);
  ezxml_free(caps);
  capheader=
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\
<!DOCTYPE WMT_MS_Capabilities SYSTEM \"http://schemas.opengis.net/wms/1.1.0/capabilities_1_1_0.dtd\"\
[\
 <!ELEMENT VendorSpecificCapabilities EMPTY>\
]>\n";
  request->request.capabilities = apr_pstrcat(ctx->pool,capheader,tmpcaps,NULL);
  free(tmpcaps);
  request->request.mime_type = apr_pstrdup(ctx->pool,"text/xml");
}

/**
 * \brief parse a WMS request
 * \private \memberof mapcache_service_wms
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_wms_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *pathinfo, apr_table_t *params, mapcache_cfg *config)
{
  const char *str = NULL;
  const char *srs=NULL;
  int width=0, height=0;
  double *tmpbbox;
  mapcache_extent extent;
  int isGetMap=0,iswms130=0;
  int errcode = 200;
  char *errmsg = NULL;
  mapcache_service_wms *wms_service = (mapcache_service_wms*)this;

  *request = NULL;
  str = apr_table_get(params,"SERVICE");
  if(!str) {
    /* service is optional if we have a getmap */
    str = apr_table_get(params,"REQUEST");
    if(!str) {
      errcode = 400;
      errmsg = "received wms with no service and request";
      ctx->service = NULL;
      goto proxies;
    }
  } else if( strcasecmp(str,"wms") ) {
    errcode = 400;
    errmsg = apr_psprintf(ctx->pool,"received wms request with invalid service param %s", str);
    goto proxies;
  }

  str = apr_table_get(params,"REQUEST");
  if(!str) {
    errcode = 400;
    errmsg = "received wms with no request";
    goto proxies;
  }

  if( ! strcasecmp(str,"getmap")) {
    isGetMap = 1;
    str = apr_table_get(params,"VERSION");
    if(str && !strcmp(str,"1.3.0")) {
      iswms130 = 1;
    }
  } else {
    if( ! strcasecmp(str,"getcapabilities") ) {
      *request = (mapcache_request*)
                 apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_capabilities_wms));
      (*request)->type = MAPCACHE_REQUEST_GET_CAPABILITIES;
      goto proxies; /* OK */
    } else if( ! strcasecmp(str,"getfeatureinfo") ) {
      //nothing
    } else {
      errcode = 501;
      errmsg = apr_psprintf(ctx->pool,"received wms with invalid request %s",str);
      goto proxies;
    }
  }


  str = apr_table_get(params,"BBOX");
  if(!str) {
    errcode = 400;
    errmsg = "received wms request with no bbox";
    goto proxies;
  } else {
    int nextents;
    if(MAPCACHE_SUCCESS != mapcache_util_extract_double_list(ctx, str,",",&tmpbbox,&nextents) ||
        nextents != 4) {
      errcode = 400;
      errmsg = "received wms request with invalid bbox";
      goto proxies;
    }
    extent.minx = tmpbbox[0];
    extent.miny = tmpbbox[1];
    extent.maxx = tmpbbox[2];
    extent.maxy = tmpbbox[3];
  }

  str = apr_table_get(params,"WIDTH");
  if(!str) {
    errcode = 400;
    errmsg = "received wms request with no width";
    goto proxies;
  } else {
    char *endptr;
    width = (int)strtol(str,&endptr,10);
    if(*endptr != 0 || width <= 0) {
      errcode = 400;
      errmsg = "received wms request with invalid width";
      goto proxies;
    }
  }

  str = apr_table_get(params,"HEIGHT");
  if(!str) {
    errcode = 400;
    errmsg = "received wms request with no height";
    goto proxies;
  } else {
    char *endptr;
    height = (int)strtol(str,&endptr,10);
    if(*endptr != 0 || height <= 0) {
      errcode = 400;
      errmsg = "received wms request with invalid height";
      goto proxies;
    }
  }

  if(width > wms_service->maxsize || height > wms_service->maxsize) {
    errcode=400;
    errmsg = "received wms request with width or height over configured maxsize limit";
    goto proxies;
  }

  if(iswms130) {
    srs = apr_table_get(params,"CRS");
    if(!srs) {
      errcode = 400;
      errmsg = "received wms request with no crs";
      goto proxies;
    }
  } else {
    srs = apr_table_get(params,"SRS");
    if(!srs) {
      errcode = 400;
      errmsg = "received wms request with no srs";
      goto proxies;
    }
  }
  if(iswms130) {
    /*check if we should flip the axis order*/
    if(mapcache_is_axis_inverted(srs)) {
      double swap;
      swap = extent.minx;
      extent.minx = extent.miny;
      extent.miny = swap;
      swap = extent.maxx;
      extent.maxx = extent.maxy;
      extent.maxy = swap;
    }
  }

  if(isGetMap) {
    str = apr_table_get(params,"LAYERS");
    if(!str) {
      errcode = 400;
      errmsg = "received wms request with no layers";
      goto proxies;
    } else {
      char *last, *layers;
      const char *key;
      int count=1;
      int nallocated = 0;
      int i,layeridx;
      int x,y,z;
      mapcache_request_get_map *map_req = NULL;
      mapcache_request_get_tile *tile_req = NULL;
      mapcache_grid_link *main_grid_link = NULL;
      mapcache_tileset *main_tileset = NULL;
      mapcache_request_type type;

      /* count the number of layers that are requested.
       * if we are in combined-mirror mode, then there is
       * always a single layer */
      if(config->mode != MAPCACHE_MODE_MIRROR_COMBINED) {
        for(key=str; *key; key++) if(*key == ',') count++;
      }

      /*
       * look to see if we have a getTile or a getMap request. We do this by looking at the first
       * wms layer that was provided in the request.
       * Checking to see if all requested layers reference the grid will be done in a second step
       */
      type = MAPCACHE_REQUEST_GET_TILE;

      if(count ==1 || config->mode == MAPCACHE_MODE_MIRROR_COMBINED) {
        key = str;
      } else {
        layers = apr_pstrdup(ctx->pool,str);
        key = apr_strtok(layers, ",", &last); /* extract first layer */
      }
      main_tileset = mapcache_configuration_get_tileset(config,key);
      if(!main_tileset) {
        errcode = 404;
        errmsg = apr_psprintf(ctx->pool,"received wms request with invalid layer %s", key);
        goto proxies;
      }
      if(config->mode != MAPCACHE_MODE_NORMAL) {
        main_tileset = mapcache_tileset_clone(ctx,main_tileset);
        main_tileset->name = (char*)key;
      }

      for(i=0; i<main_tileset->grid_links->nelts; i++) {
        mapcache_grid_link *sgrid = APR_ARRAY_IDX(main_tileset->grid_links,i,mapcache_grid_link*);
        /* look for a grid with a matching srs */
        if(strcasecmp(sgrid->grid->srs,srs)) {
          /* look if the grid has some srs aliases */
          int s;
          for(s=0; s<sgrid->grid->srs_aliases->nelts; s++) {
            char *srsalias = APR_ARRAY_IDX(sgrid->grid->srs_aliases,s,char*);
            if(!strcasecmp(srsalias,srs)) break;
          }
          if(s==sgrid->grid->srs_aliases->nelts)
            continue; /* no srs alias matches the requested srs */
        }
        main_grid_link = sgrid;
        break;
      }
      if(!main_grid_link) {
        errcode = 400;
        errmsg = apr_psprintf(ctx->pool,
                              "received unsuitable wms request: no <grid> with suitable srs found for layer %s",main_tileset->name);
        goto proxies;
      }

      /* verify we align on the tileset's grid */
      if(main_grid_link->grid->tile_sx != width || main_grid_link->grid->tile_sy != height ||
          mapcache_grid_get_cell(ctx, main_grid_link->grid, &extent, &x,&y,&z) != MAPCACHE_SUCCESS) {
        /* we have the correct srs, but the request does not align on the grid */
        type = MAPCACHE_REQUEST_GET_MAP;
      }


      if(type == MAPCACHE_REQUEST_GET_TILE) {
        tile_req = apr_pcalloc(ctx->pool, sizeof(mapcache_request_get_tile));
        tile_req->request.type = MAPCACHE_REQUEST_GET_TILE;
        tile_req->tiles = apr_pcalloc(ctx->pool, count*sizeof(mapcache_tile*));
        tile_req->format = wms_service->getmap_format;
        *request = (mapcache_request*)tile_req;
      } else {
        map_req = apr_pcalloc(ctx->pool, sizeof(mapcache_request_get_map));
        map_req->request.type = MAPCACHE_REQUEST_GET_MAP;
        map_req->maps = apr_pcalloc(ctx->pool, count*sizeof(mapcache_map*));
        map_req->getmap_strategy = wms_service->getmap_strategy;
        map_req->resample_mode = wms_service->resample_mode;
        map_req->getmap_format = wms_service->getmap_format;
        *request = (mapcache_request*)map_req;
      }
      nallocated = count;

      /*
       * loop through all the layers to verify that they reference the requested grid,
       * and to extract any dimensions if configured
       */
      if(count>1)
        layers = apr_pstrdup(ctx->pool,str); /* apr_strtok modifies its input string */

      for (layeridx=0,key = ((count==1)?str:apr_strtok(layers, ",", &last)); key != NULL;
           key = ((count==1)?NULL:apr_strtok(NULL, ",", &last)),layeridx++) {
        int i;
        mapcache_tileset *tileset = main_tileset;
        mapcache_grid_link *grid_link = main_grid_link;
        apr_table_t *dimtable = NULL;

        if(layeridx) {
          /*
           * if we have multiple requested layers, check that they reference the requested grid
           * this step is not done for the first tileset as we have already performed it
           */
          tileset = mapcache_configuration_get_tileset(config,key);
          if (!tileset) {
            errcode = 404;
            errmsg = apr_psprintf(ctx->pool,"received wms request with invalid layer %s", key);
            goto proxies;
          }
          if(config->mode != MAPCACHE_MODE_NORMAL) {
            tileset = mapcache_tileset_clone(ctx,tileset);
            tileset->name = (char*)key;
          }
          grid_link = NULL;
          for(i=0; i<tileset->grid_links->nelts; i++) {
            grid_link = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
            if(grid_link->grid == main_grid_link->grid) {
              break;
            }
          }
          if(i==tileset->grid_links->nelts) {
            /* the tileset does not reference the grid of the first tileset */
            errcode = 400;
            errmsg = apr_psprintf(ctx->pool,
                                  "tileset %s does not reference grid %s (referenced by tileset %s)",
                                  tileset->name, main_grid_link->grid->name,main_tileset->name);
            goto proxies;
          }
        }
        if(type == MAPCACHE_REQUEST_GET_TILE) {
          mapcache_tile *tile = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
          tile->x = x;
          tile->y = y;
          tile->z = z;
          mapcache_tileset_tile_validate(ctx,tile);
          if(GC_HAS_ERROR(ctx)) {
            /* don't bail out just yet, in case multiple tiles have been requested */
            ctx->clear_errors(ctx);
          } else {
            tile_req->tiles[tile_req->ntiles++] = tile;
          }
          dimtable = tile->dimensions;

        } else {
          mapcache_map *map = mapcache_tileset_map_create(ctx->pool,tileset,grid_link);
          map->width = width;
          map->height = height;
          map->extent = extent;
          map_req->maps[map_req->nmaps++] = map;
          dimtable = map->dimensions;
        }

        /*look for dimensions*/
        if(dimtable) {
          const char *value;
          if(tileset->dimensions) {
            for(i=0; i<tileset->dimensions->nelts; i++) {
              mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
              if((value = (char*)apr_table_get(params,dimension->name)) != NULL) {
                char *tmpval = apr_pstrdup(ctx->pool,value);
                int ok = dimension->validate(ctx,dimension,&tmpval);
                GC_CHECK_ERROR(ctx);
                if(ok == MAPCACHE_SUCCESS)
                  apr_table_setn(dimtable,dimension->name,tmpval);
                else {
                  errcode = 400;
                  errmsg = apr_psprintf(ctx->pool, "dimension \"%s\" value \"%s\" fails to validate",
                                        dimension->name, value);
                  goto proxies;
                }
              }
            }
          }
          if(tileset->timedimension) {
          /* possibly duplicate the created map/tile for each entry of the requested time dimension */
            apr_array_header_t *timedim_selected;
            value = apr_table_get(params,tileset->timedimension->key);
            if(!value)
              value = tileset->timedimension->default_value;
            timedim_selected = mapcache_timedimension_get_entries_for_value(ctx,
                    tileset->timedimension, tileset, grid_link->grid, &extent, value);
            GC_CHECK_ERROR(ctx);
            if(!timedim_selected || timedim_selected->nelts == 0) {
              errcode = 404;
              errmsg = apr_psprintf(ctx->pool,"no matching entry for given TIME dimension \"%s\" in tileset \"%s\"",
                      tileset->timedimension->key, tileset->name);
              goto proxies;
            }
            if(type == MAPCACHE_REQUEST_GET_TILE) {
              int i;
            /* we need to create more tile/map entries */
              if(timedim_selected->nelts > 1) {
                /* apr pools have no realloc */
                nallocated = nallocated + timedim_selected->nelts - 1;
                mapcache_tile** tmptiles =
                        apr_palloc(ctx->pool, nallocated * sizeof(mapcache_tile*));
                for(i=0;i<tile_req->ntiles;i++) {
                  tmptiles[i] = tile_req->tiles[i];
                }
                tile_req->tiles = tmptiles;
                /* end realloc workaround */
              }
              for(i=0;i<timedim_selected->nelts;i++) {
                if(i) {
                  tile_req->tiles[tile_req->ntiles] =
                          mapcache_tileset_tile_clone(ctx->pool,tile_req->tiles[tile_req->ntiles-1]);
                  tile_req->ntiles++;
                }
                apr_table_set(tile_req->tiles[tile_req->ntiles-1]->dimensions,tileset->timedimension->key,
                        APR_ARRAY_IDX(timedim_selected,i,char*));
              }
            } else {
              int i;
            /* we need to create more tile/map entries */
              if(timedim_selected->nelts > 1) {
                /* apr pools have no realloc */
                nallocated = nallocated + timedim_selected->nelts - 1;
                mapcache_map** tmpmaps =
                        apr_palloc(ctx->pool, nallocated * sizeof(mapcache_map*));
                for(i=0;i<map_req->nmaps;i++) {
                  tmpmaps[i] = map_req->maps[i];
                }
                map_req->maps = tmpmaps;
                /* end realloc workaround */
              }
              for(i=0;i<timedim_selected->nelts;i++) {
                if(i) {
                  map_req->maps[map_req->nmaps] =
                          mapcache_tileset_map_clone(ctx->pool,map_req->maps[map_req->nmaps-1]);
                  map_req->nmaps++;
                }
                apr_table_set(map_req->maps[map_req->nmaps-1]->dimensions,tileset->timedimension->key,
                        APR_ARRAY_IDX(timedim_selected,i,char*));
              }
            }
          }
        }
      }
      if(tile_req && tile_req->ntiles == 0) {
        errcode = 404;
        errmsg = "request for tile outside of restricted extent";
        goto proxies;
      }
    }
  } else {
    int i;
    int x,y;
    mapcache_grid_link *grid_link;
    mapcache_feature_info *fi;
    mapcache_request_get_feature_info *req_fi;
    //getfeatureinfo
    str = apr_table_get(params,"QUERY_LAYERS");
    if(!str) {
      errcode = 400;
      errmsg = "received wms getfeatureinfo request with no query layers";
      goto proxies;
    } else if(strstr(str,",")) {
      errcode = 501;
      errmsg = "wms getfeatureinfo not implemented for multiple layers";
      goto proxies;
    } else {
      mapcache_tileset *tileset = mapcache_configuration_get_tileset(config,str);
      if(!tileset) {
        errcode = 404;
        errmsg = apr_psprintf(ctx->pool,"received wms getfeatureinfo request with invalid layer %s", str);
        goto proxies;
      }
      if(!tileset->source || !tileset->source->info_formats) {
        errcode = 404;
        errmsg = apr_psprintf(ctx->pool,"received wms getfeatureinfo request for unqueryable layer %s", str);
        goto proxies;
      }

      grid_link = NULL;
      for(i=0; i<tileset->grid_links->nelts; i++) {
        mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
        if(strcasecmp(sgrid->grid->srs,srs)) continue;
        grid_link = sgrid;
        break;
      }
      if(!grid_link) {
        errcode = 400;
        errmsg = apr_psprintf(ctx->pool,
                              "received unsuitable wms request: no <grid> with suitable srs found for layer %s",tileset->name);
        goto proxies;
      }

      str = apr_table_get(params,"X");
      if(!str) {
        errcode = 400;
        errmsg = "received wms getfeatureinfo request with no X";
        goto proxies;
      } else {
        char *endptr;
        x = (int)strtol(str,&endptr,10);
        if(*endptr != 0 || x <= 0 || x>=width) {
          errcode = 400;
          errmsg = "received wms request with invalid X";
          goto proxies;
        }
      }

      str = apr_table_get(params,"Y");
      if(!str) {
        errcode = 400;
        errmsg = "received wms getfeatureinfo request with no Y";
        goto proxies;
      } else {
        char *endptr;
        y = (int)strtol(str,&endptr,10);
        if(*endptr != 0 || y <= 0 || y>=height) {
          errcode = 400;
          errmsg = "received wms request with invalid Y";
          goto proxies;
        }
      }

      fi = mapcache_tileset_feature_info_create(ctx->pool, tileset, grid_link);
      fi->i = x;
      fi->j = y;
      fi->format = apr_pstrdup(ctx->pool,apr_table_get(params,"INFO_FORMAT"));
      if(!fi->format) {
        errcode = 400;
        errmsg = "received wms getfeatureinfo request with no INFO_FORMAT";
        goto proxies;
      }

      if(fi->map.dimensions) {
        int i;
        for(i=0; i<tileset->dimensions->nelts; i++) {
          mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
          const char *value;
          if((value = (char*)apr_table_get(params,dimension->name)) != NULL) {
            char *tmpval = apr_pstrdup(ctx->pool,value);
            int ok = dimension->validate(ctx,dimension,&tmpval);
            GC_CHECK_ERROR(ctx);
            if(ok == MAPCACHE_SUCCESS)
              apr_table_setn(fi->map.dimensions,dimension->name,tmpval);
            else {
              errcode = 400;
              errmsg = apr_psprintf(ctx->pool,"dimension \"%s\" value \"%s\" fails to validate",
                                    dimension->name, value);
              goto proxies;
            }
          }
        }
      }
      fi->map.width = width;
      fi->map.height = height;
      fi->map.extent = extent;
      req_fi = apr_pcalloc(ctx->pool, sizeof(mapcache_request_get_feature_info));
      req_fi->request.type = MAPCACHE_REQUEST_GET_FEATUREINFO;
      req_fi->fi = fi;
      *request = (mapcache_request*)req_fi;

    }
  }

proxies:
  /*
   * if we don't have a gettile or getmap request we can treat from the cache tiles, look to see if we have a rule
   * that tells us to forward the request somewhere else
   */
  if(errcode == 200 &&
      *request && (
        /* if its a single tile we're ok*/
        ((*request)->type == MAPCACHE_REQUEST_GET_TILE && ((mapcache_request_get_tile*)(*request))->ntiles == 1) ||
        ((*request)->type == MAPCACHE_REQUEST_GET_FEATUREINFO) ||

        /* if we have a getmap or multiple tiles, we must check that assembling is allowed */
        (((*request)->type == MAPCACHE_REQUEST_GET_MAP || (
            (*request)->type == MAPCACHE_REQUEST_GET_TILE && ((mapcache_request_get_tile*)(*request))->ntiles > 1)) &&
         wms_service->getmap_strategy == MAPCACHE_GETMAP_ASSEMBLE)
      )) {
    /* if we're here, then we have succesfully parsed the request and can treat it ourselves, i.e. from cached tiles */
    return;
  } else {
    /* look to see if we can proxy the request somewhere*/
    int i,j;
    for(i=0; i<wms_service->forwarding_rules->nelts; i++) {
      mapcache_forwarding_rule *rule = APR_ARRAY_IDX(wms_service->forwarding_rules,i,mapcache_forwarding_rule*);
      int got_a_match = 1;
      for(j=0; j<rule->match_params->nelts; j++) {
        mapcache_dimension *match_param = APR_ARRAY_IDX(rule->match_params,j,mapcache_dimension*);
        const char *value = apr_table_get(params,match_param->name);
        if(!value || match_param->validate(ctx,match_param,(char**)&value) == MAPCACHE_FAILURE) {
          /* the parameter was not supplied, or did not validate: we don't apply this rule */
          got_a_match = 0;
          break;
        }
      }
      if( got_a_match == 1 ) {
        mapcache_request_proxy *req_proxy = apr_pcalloc(ctx->pool,sizeof(mapcache_request_proxy));
        *request = (mapcache_request*)req_proxy;
        (*request)->service = this;
        (*request)->type = MAPCACHE_REQUEST_PROXY;
        req_proxy->http = rule->http;
        req_proxy->params = params;
        if(rule->append_pathinfo) {
          req_proxy->pathinfo = pathinfo;
        } else {
          req_proxy->pathinfo = NULL;
        }
        return;
      }
    }
  }

  /*
   * if we are here, then we are either in the getfeatureinfo / getcapabilities / getmap case,
   * or there was an error parsing the request and no rules to proxy it elsewhere
   */
  if(errcode != 200) {
    ctx->set_error(ctx,errcode,errmsg);
    return;
  }
#ifdef DEBUG
  if((*request)->type != MAPCACHE_REQUEST_GET_TILE &&
      (*request)->type != MAPCACHE_REQUEST_GET_MAP &&
      (*request)->type != MAPCACHE_REQUEST_GET_FEATUREINFO &&
      (*request)->type != MAPCACHE_REQUEST_GET_CAPABILITIES) {
    ctx->set_error(ctx,500,"BUG: request not gettile or getmap");
    return;
  }
#endif
}


void _configuration_parse_wms_xml(mapcache_context *ctx, ezxml_t node, mapcache_service *gservice, mapcache_cfg *cfg)
{
  mapcache_service_wms *wms = (mapcache_service_wms*)gservice;
  ezxml_t rule_node;
  assert(gservice->type == MAPCACHE_SERVICE_WMS);

  for( rule_node = ezxml_child(node,"forwarding_rule"); rule_node; rule_node = rule_node->next) {
    mapcache_forwarding_rule *rule;
    ezxml_t http_node;
    ezxml_t pathinfo_node;
    ezxml_t param_node;
    char *name = (char*)ezxml_attr(rule_node,"name");
    if(!name) name = "(null)";
    rule = apr_pcalloc(ctx->pool, sizeof(mapcache_forwarding_rule));
    rule->name = apr_pstrdup(ctx->pool,name);
    rule->match_params = apr_array_make(ctx->pool,1,sizeof(mapcache_dimension*));

    pathinfo_node = ezxml_child(rule_node,"append_pathinfo");
    if(pathinfo_node && !strcasecmp(pathinfo_node->txt,"true")) {
      rule->append_pathinfo = 1;
    } else {
      rule->append_pathinfo = 0;
    }
    http_node = ezxml_child(rule_node,"http");
    if(!http_node) {
      ctx->set_error(ctx,500,"rule \"%s\" does not contain an <http> block",name);
      return;
    }
    rule->http = mapcache_http_configuration_parse_xml(ctx,http_node);
    GC_CHECK_ERROR(ctx);

    for(param_node = ezxml_child(rule_node,"param"); param_node; param_node = param_node->next) {
      char *name = (char*)ezxml_attr(param_node,"name");
      char *type = (char*)ezxml_attr(param_node,"type");

      mapcache_dimension *dimension = NULL;

      if(!name || !strlen(name)) {
        ctx->set_error(ctx, 400, "mandatory attribute \"name\" not found in forwarding rule <param>");
        return;
      }

      if(type && *type) {
        if(!strcmp(type,"values")) {
          dimension = mapcache_dimension_values_create(ctx->pool);
        } else if(!strcmp(type,"regex")) {
          dimension = mapcache_dimension_regex_create(ctx->pool);
        } else {
          ctx->set_error(ctx,400,"unknown <param> type \"%s\". expecting \"values\" or \"regex\".",type);
          return;
        }
      } else {
        ctx->set_error(ctx,400, "mandatory attribute \"type\" not found in <dimensions>");
        return;
      }

      dimension->name = apr_pstrdup(ctx->pool,name);

      dimension->configuration_parse_xml(ctx,dimension,param_node);
      GC_CHECK_ERROR(ctx);

      APR_ARRAY_PUSH(rule->match_params,mapcache_dimension*) = dimension;
    }
    APR_ARRAY_PUSH(wms->forwarding_rules,mapcache_forwarding_rule*) = rule;
  }
  if ((rule_node = ezxml_child(node,"full_wms")) != NULL) {
    if(!strcmp(rule_node->txt,"assemble")) {
      wms->getmap_strategy = MAPCACHE_GETMAP_ASSEMBLE;
    } else if(!strcmp(rule_node->txt,"forward")) {
      wms->getmap_strategy = MAPCACHE_GETMAP_FORWARD;
    } else if(*rule_node->txt && strcmp(rule_node->txt,"error")) {
      ctx->set_error(ctx,400, "unknown value %s for node <full_wms> (allowed values: assemble, getmap or error", rule_node->txt);
      return;
    }
  }

  wms->getmap_format = mapcache_configuration_get_image_format(cfg,"JPEG");
  if ((rule_node = ezxml_child(node,"format")) != NULL) {
    wms->getmap_format = mapcache_configuration_get_image_format(cfg,rule_node->txt);
    if(!wms->getmap_format) {
      ctx->set_error(ctx,400, "unknown <format> %s for wms service", rule_node->txt);
      return;
    }
  }

  if ((rule_node = ezxml_child(node,"resample_mode")) != NULL) {
    if(!strcmp(rule_node->txt,"nearest")) {
      wms->resample_mode = MAPCACHE_RESAMPLE_NEAREST;
    } else if(!strcmp(rule_node->txt,"bilinear")) {
      wms->resample_mode = MAPCACHE_RESAMPLE_BILINEAR;
    } else {
      ctx->set_error(ctx,400, "unknown value %s for node <resample_mode> (allowed values: nearest, bilinear", rule_node->txt);
      return;
    }
  }

  if ((rule_node = ezxml_child(node,"maxsize")) != NULL) {
    wms->maxsize = atoi(rule_node->txt);
    if(wms->maxsize <= 0) {
      ctx->set_error(ctx,400, "failed to parse wms service maxsize value \"%s\"", rule_node->txt);
      return;
    }
  }
}

void _format_error_wms(mapcache_context *ctx, mapcache_service *service, char *msg,
                       char **err_body, apr_table_t *headers)
{
  char *template = "\
<?xml version='1.0' encoding=\"UTF-8\" standalone=\"no\" ?>\n\
<!DOCTYPE ServiceExceptionReport SYSTEM \
\"http://schemas.opengis.net/wms/1.1.1/exception_1_1_1.dtd\">\n\
<ServiceExceptionReport version=\"1.1.1\">\n\
<ServiceException>\n\
<![CDATA[\n\
%s\n\
]]>\n\
</ServiceException>\n\
%s\
</ServiceExceptionReport>";

  char *exceptions="";

  if(ctx->exceptions) {
    const apr_array_header_t *array = apr_table_elts(ctx->exceptions);
    apr_table_entry_t *elts = (apr_table_entry_t *) array->elts;
    int i;
    for (i = 0; i < array->nelts; i++) {
      exceptions = apr_pstrcat(ctx->pool,exceptions,apr_psprintf(ctx->pool,
                               "<ServiceException code=\"%s\"><![CDATA[%s]]></ServiceException>\n",elts[i].key,elts[i].val),NULL);
    }
  }

  *err_body = apr_psprintf(ctx->pool,template,msg,exceptions);
  apr_table_set(headers, "Content-Type", "application/vnd.ogc.se_xml");
}

mapcache_service* mapcache_service_wms_create(mapcache_context *ctx)
{
  mapcache_service_wms* service = (mapcache_service_wms*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_wms));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate wms service");
    return NULL;
  }
  service->forwarding_rules = apr_array_make(ctx->pool,0,sizeof(mapcache_forwarding_rule*));
  service->maxsize=2048;
  service->service.url_prefix = apr_pstrdup(ctx->pool,"");
  service->service.name = apr_pstrdup(ctx->pool,"wms");
  service->service.type = MAPCACHE_SERVICE_WMS;
  service->service.parse_request = _mapcache_service_wms_parse_request;
  service->service.create_capabilities_response = _create_capabilities_wms;
  service->service.configuration_parse_xml = _configuration_parse_wms_xml;
  service->service.format_error = _format_error_wms;
  service->getmap_strategy = MAPCACHE_GETMAP_ASSEMBLE;
  service->resample_mode = MAPCACHE_RESAMPLE_BILINEAR;
  service->getmap_format = NULL;
  return (mapcache_service*)service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
