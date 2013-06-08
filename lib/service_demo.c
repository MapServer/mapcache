/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching demo service implementation
 * Author:   Thomas Bonfort, Stephen Woodbridge and the MapServer team.
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

#include <ctype.h>
#include "mapcache.h"
#include <apr_strings.h>
#include <math.h>
#include <apr_tables.h>

/** \addtogroup services */
/** @{ */

static char *demo_head =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "  <head>\n"
  "    <meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\" />\n"
  "    <title>mod-mapcache demo service</title>\n"
  "    <style type=\"text/css\">\n"
  "    html, body {\n"
  "        height: 100%;\n"
  "        width: 100%;\n"
  "        border: 0px;\n"
  "        margin: 0px;\n"
  "        padding: 0px;\n"
  "    }\n"
  "    #map {\n"
  "        width: calc(100%% - 2px);\n"
  "        height: calc(100%% - 2px);\n"
  "        border: 1px solid black;\n"
  "        margin: 0px;\n"
  "        padding: 0px;\n"
  "    }\n"
  "    </style>\n"
  "    <script src=\"http://www.openlayers.org/api/OpenLayers.js\"></script>\n"
  "    <script type=\"text/javascript\">\n"
  "%s\n"
  "var map;\n"
  "function init(){\n"
  "    map = new OpenLayers.Map( 'map', {\n"
  "        displayProjection: new OpenLayers.Projection(\"EPSG:4326\")\n"
  "    } );\n";

static char *demo_ve_extra =
  "function QuadTree(tx, ty, zoom) {\n"
  "    var tname = '';\n"
  "    var i, j, mask, digit;\n"
  "    var zero = '0'.charCodeAt(0);\n"
  "\n"
  "    if (ty < 0) ty = 2 - ty;\n"
  "    ty = (Math.pow(2,zoom) - 1) - ty;\n"
  "\n"
  "    for (i=zoom, j=0; i>0; i--, j++) {\n"
  "        digit = 0;\n"
  "        mask = 1 << (i-1);\n"
  "        if (tx & mask) digit += 1;\n"
  "        if (ty & mask) digit += 2;\n"
  "        tname += String.fromCharCode(zero + digit);\n"
  "    }\n"
  "    return tname;\n"
  "}\n"
  "    function WGSQuadTree(tx, ty, zoom) {\n"
  "        var tname = '';\n"
  "        var i, n;\n"
  "        var zero = '0'.charCodeAt(0);\n"
  "\n"
  "        ty = (Math.pow(2,zoom-1) - 1) - ty;\n"
  "\n"
  "        for (i=zoom; i>0; i--) {\n"
  "            if (i == 1)\n"
  "                n = Math.floor(tx/2)*4 + tx%2 + (ty%2)*2;\n"
  "            else\n"
  "                n = (ty%2)*2 + tx%2;\n"
  "            if (n<0 || n>9)\n"
  "                return '';\n"
  "            tname = String.fromCharCode(zero + n) + tname;\n"
  "            tx = Math.floor(tx/2);\n"
  "            ty = Math.floor(ty/2);\n"
  "        }\n"
  "        return tname;\n"
  "    }\n"
  "function get_ve_url (bounds) {\n"
  "    var xoriginShift, yoriginShift, id;\n"
  "    if (this.sphericalMercator) {\n"
  "        xoriginShift = 2 * Math.PI * 6378137.0 / 2.0; // meters\n"
  "        yoriginShift = xoriginShift;\n"
  "    }\n"
  "    else {\n"
  "        xoriginShift = 180.0;\n"
  "        yoriginShift = 90;\n"
  "    }\n"
  "\n"
  "    var res = this.map.getResolution();\n"
  "    var x = Math.floor(Math.ceil(((bounds.left + bounds.right)/2.0 + xoriginShift) / res / this.tileSize.w) - 1);\n"
  "    var y = Math.floor(Math.ceil(((bounds.top + bounds.bottom)/2.0 + yoriginShift) / res / this.tileSize.h) - 1);\n"
  "    var z = this.map.getZoom();\n"
  "    if (this.sphericalMercator) {\n"
  "        id = QuadTree(x, y, z);\n"
  "    }\n"
  "    else {\n"
  "        id = WGSQuadTree(x, y, z);\n"
  "    }\n"
  "    var path = '?LAYER=' + this.options.layername + '&tile=' + id;\n"
  "    var url = this.url;\n"
  "    if (url instanceof Array) {\n"
  "        url = this.selectUrl(path, url);\n"
  "    }\n"
  "    return url + path;\n"
  "}\n";

static char *demo_layer_wms =
  "    var %s_wms_layer = new OpenLayers.Layer.WMS( \"%s-%s-WMS\",\n"
  "        \"%s\",{layers: '%s'},\n"
  "        { gutter:0,buffer:0,isBaseLayer:true,transitionEffect:'resize',\n"
  "          resolutions:[%s],\n"
  "          units:\"%s\",\n"
  "          maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "          projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "          sphericalMercator: %s\n"
  "        }\n"
  "    );\n"
  "    map.addLayer(%s_wms_layer);\n\n";

static char *demo_layer_tms =
  "    var %s_tms_layer = new OpenLayers.Layer.TMS( \"%s-%s-TMS\",\n"
  "        \"%s\",\n"
  "        { layername: '%s@%s', type: \"%s\", serviceVersion:\"1.0.0\",\n"
  "          gutter:0,buffer:0,isBaseLayer:true,transitionEffect:'resize',\n"
  "          tileOrigin: new OpenLayers.LonLat(%f,%f),\n"
  "          resolutions:[%s],\n"
  "          zoomOffset:%d,\n"
  "          units:\"%s\",\n"
  "          maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "          projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "          sphericalMercator: %s\n"
  "        }\n"
  "    );\n"
  "    map.addLayer(%s_tms_layer);\n\n";


static char *demo_layer_wmts =
  "    var %s_wmts_layer = new OpenLayers.Layer.WMTS({\n"
  "        name: \"%s-%s-WMTS\",\n"
  "        url: \"%s\",\n"
  "        layer: '%s',\n"
  "        matrixSet: '%s',\n"
  "        format: '%s',\n"
  "        style: 'default',\n"
  "        gutter:0,buffer:0,isBaseLayer:true,transitionEffect:'resize',\n"
  "        resolutions:[%s],\n"
  "        zoomOffset:%d,\n"
  "        units:\"%s\",\n"
  "        maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "        projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "        sphericalMercator: %s\n"
  "      }\n"
  "    );\n"
  "    map.addLayer(%s_wmts_layer);\n\n";

static char *demo_layer_ve =
  "    var %s_ve_layer = new OpenLayers.Layer.TMS( \"%s-%s-VE\",\n"
  "        \"%s\",\n"
  "        { layername: '%s@%s',\n"
  "          getURL: get_ve_url,\n"
  "          gutter:0,buffer:0,isBaseLayer:true,transitionEffect:'resize',\n"
  "          resolutions:[%s],\n"
  "          units:\"%s\",\n"
  "          maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "          projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "          sphericalMercator: %s\n"
  "        }\n"
  "    );\n"
  "    map.addLayer(%s_ve_layer);\n\n";

static char *demo_layer_singletile =
  "    var %s_slayer = new OpenLayers.Layer.WMS( \"%s-%s (singleTile)\",\n"
  "        \"%s\",{layers: '%s'},\n"
  "        { gutter:0,ratio:1,isBaseLayer:true,transitionEffect:'resize',\n"
  "          resolutions:[%s],\n"
  "          units:\"%s\",\n"
  "          singleTile:true,\n"
  "          maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "          projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "          sphericalMercator: %s\n"
  "        }\n"
  "    );\n"
  "    map.addLayer(%s_slayer);\n\n";

static char *demo_control_featureinfo =
  "    var %s_info = new OpenLayers.Control.WMSGetFeatureInfo({\n"
  "      url: '%s',\n"
  "      infoFormat: '%s',\n"
  "      title: 'Identify features by clicking',\n"
  "      queryVisible: true,\n"
  "      eventListeners: {\n"
  "        getfeatureinfo: function(event) {\n"
  "            map.addPopup(new OpenLayers.Popup.FramedCloud(\n"
  "                'chicken',\n"
  "                map.getLonLatFromPixel(event.xy),\n"
  "                null,\n"
  "                event.text,\n"
  "                null,\n"
  "                true\n"
  "            ));\n"
  "        }\n"
  "      }\n"
  "    });\n"
  "    map.addControl(%s_info);\n"
  "    %s_info.activate();\n\n";


static char *demo_footer =
  "%s"
  "    if(!map.getCenter())\n"
  "        map.zoomToMaxExtent();\n"
  "    map.addControl(new OpenLayers.Control.LayerSwitcher());\n"
  "    map.addControl(new OpenLayers.Control.MousePosition());\n"
  "}\n"
  "    </script>\n"
  "  </head>\n"
  "\n"
  "<body onload=\"init()\">\n"
  "    <div id=\"map\">\n"
  "    </div>\n"
  "</body>\n"
  "</html>\n";

static char *demo_head_gmaps =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\" />\n"
  "<meta name=\"viewport\" content=\"initial-scale=1.0, user-scalable=no\" />\n"
  "<title>mod_mapcache gmaps demo</title>\n"
  "<style type=\"text/css\">\n"
  "  html { height: 100% }\n"
  "  body { height: 100%; margin: 0px; padding: 0px }\n"
  "  #map_canvas { height: 100% }\n"
  "</style>\n"
  "<script type=\"text/javascript\"\n"
  "    src=\"http://maps.google.com/maps/api/js?sensor=false\">\n"
  "</script>\n"
  "<script type=\"text/javascript\">\n"
  "  // Normalize the coords so that they repeat horizontally\n"
  "  // like standard google maps\n"
  "  function getNormalizedCoord(coord, zoom) {\n"
  "    var y = coord.y;\n"
  "    var x = coord.x;\n"
  "\n"
  "    // tile range in one direction\n"
  "    // 0 = 1 tile, 1 = 2 tiles, 2 = 4 tiles, 3 = 8 tiles, etc.\n"
  "    var tileRange = 1 << zoom;\n"
  "\n"
  "    // don't repeat across y-axis (vertically)\n"
  "    if (y < 0 || y >= tileRange) {\n"
  "      return null;\n"
  "    }\n"
  "\n"
  "    // repeat accross x-axis\n"
  "    if (x < 0 || x >= tileRange) {\n"
  "      x = (x % tileRange + tileRange) % tileRange;\n"
  "    }\n"
  "\n"
  "    return { x: x, y: y };\n"
  "  }\n"
  "\n"
  "function makeLayer(name, url, size, extension, minzoom, maxzoom) {\n"
  "  var layer = {\n"
  "    name: name,\n"
  "    TypeOptions: {\n"
  "      getTileUrl: function(coord, zoom) {\n"
  "        var normCoord = getNormalizedCoord(coord, zoom);\n"
  "        if (!normCoord) {\n"
  "          return null;\n"
  "        }\n"
  "        var bound = Math.pow(2, zoom);\n"
  "        return url+zoom+'/'+normCoord.x+'/'+(bound-normCoord.y-1)+'.'+extension;\n"
  "      },\n"
  "      tileSize: size,\n"
  "      isPng: true,\n"
  "      maxZoom: maxzoom,\n"
  "      minZoom: minzoom,\n"
  "      name: name\n"
  "    },\n"
  "    OverlayTypeOptions: {\n"
  "      getTileUrl: function(coord, zoom) {\n"
  "        var normCoord = getNormalizedCoord(coord, zoom);\n"
  "        if (!normCoord) {\n"
  "          return null;\n"
  "        }\n"
  "        var bound = Math.pow(2, zoom);\n"
  "        return url+zoom+'/'+normCoord.x+'/'+(bound-normCoord.y-1)+'.'+extension;\n"
  "      },\n"
  "      tileSize: size,\n"
  "      isPng: true,\n"
  "      maxZoom: maxzoom,\n"
  "      minZoom: minzoom,\n"
  "      opacity: 0.5,  // o=transparenty, 1=opaque\n"
  "      name: name+'_overlay'\n"
  "    }\n"
  "  };\n"
  "\n"
  "  layer.MapType = new google.maps.ImageMapType(layer.TypeOptions);\n"
  "  layer.OverlayMapType = new google.maps.ImageMapType(layer.OverlayTypeOptions);\n"
  "  layer.OverlayMapType.hide = function() {\n"
  "    if (this.map_) {\n"
  "      this.map_.overlayMapTypes.setAt(0, null);\n"
  "    }\n"
  "  };\n"
  "  layer.OverlayMapType.show = function() {\n"
  "    if (this.map_) {\n"
  "      this.map_.overlayMapTypes.setAt(0, this);\n"
  "    }\n"
  "  };\n"
  "  layer.OverlayMapType.toggle = function() {\n"
  "    if (this.map_) {\n"
  "      if (this.map_.overlayMapTypes.getAt(0)) {\n"
  "          this.hide();\n"
  "      } else {\n"
  "          this.show();\n"
  "      }\n"
  "    }\n"
  "  };\n"
  "  return layer;\n"
  "}\n"
  "\n"
  "var layers = Array();\n";

/*
 * name, baseurl, name, grid, size, size, extension
*/
static char *demo_layer_gmaps = "layers.push(makeLayer('%s %s', '%s/tms/1.0.0/%s@%s/', new google.maps.Size(%d,%d), '%s', %d, %d));\n";

static char *demo_footer_gmaps =
  "%s\n"
  "function initialize() {\n"
  "  var latlng = new google.maps.LatLng(0,0);\n"
  "  var ids = Array();\n"
  "  for (var i=0; i<layers.length; i++) {\n"
  "    ids.push(layers[i].name);\n"
  "  }\n"
  "  ids.push(google.maps.MapTypeId.ROADMAP);\n"
  "  var myOptions = {\n"
  "    zoom: 1,\n"
  "    center: latlng,\n"
  "    mapTypeControlOptions: {\n"
  "      mapTypeIds: ids\n"
  "    }\n"
  "  };\n"
  "  var map = new google.maps.Map(document.getElementById('map_canvas'),\n"
  "      myOptions);\n"
  "  var input = \"\";\n"
  "  for (var i=0; i<layers.length; i++) {\n"
  "    map.mapTypes.set(layers[i].name, layers[i].MapType);\n"
  "    layers[i].OverlayMapType.map_ = map;\n"
  "    map.overlayMapTypes.setAt(i, null);\n"
  "    input += '<input type=\"button\" value=\"'+layers[i].name+' Overlay\" onclick=\"layers['+i+'].OverlayMapType.toggle();\"></input>';\n"
  "  }\n"
  "  map.setMapTypeId(layers[0].name);\n"
  "  document.getElementById('toolbar').innerHTML = input;\n"
  "}\n"
  "\n"
  "</script>\n"
  "</head>\n"
  "<body onload=\"initialize()\">\n"
  "  <div id=\"toolbar\" style=\"width:100%; height:20px; text-align:center\">&nbsp;</div>\n"
  "  <div id=\"map_canvas\" style=\"width:100%; height:100%\"></div>\n"
  "</body>\n"
  "</html>\n";

static char *demo_head_title =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\" />\n"
  "  <title>%s</title>\n"
  "</head>\n"
  "<body>\n";

static char *demo_footer_title =
  "</body>\n"
  "</html>\n";

/**
 * \brief parse a demo request
 * \private \memberof mapcache_service_demo
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_demo_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *cpathinfo, apr_table_t *params, mapcache_cfg *config)
{
  mapcache_request_get_capabilities_demo *drequest =
    (mapcache_request_get_capabilities_demo*)apr_pcalloc(
      ctx->pool,sizeof(mapcache_request_get_capabilities_demo));
  *request = (mapcache_request*)drequest;
  (*request)->type = MAPCACHE_REQUEST_GET_CAPABILITIES;
  if(!cpathinfo || *cpathinfo=='\0' || !strcmp(cpathinfo,"/")) {
    /*we have no specified service, create the link page*/
    drequest->service = NULL;
    return;
  } else {
    int i;
    while(*cpathinfo == '/')
      cpathinfo++; /* skip the leading /'s */

    for(i=0; i<MAPCACHE_SERVICES_COUNT; i++) {
      /* loop through the services that have been configured */
      int prefixlen;
      mapcache_service *service = NULL;
      service = config->services[i];
      if(!service) continue; /* skip an unconfigured service */
      prefixlen = strlen(service->name);
      if(strncmp(service->name,cpathinfo, prefixlen)) continue; /*skip a service who's prefix does not correspond */
      if(*(cpathinfo+prefixlen)!='/' && *(cpathinfo+prefixlen)!='\0') continue; /*we matched the prefix but there are trailing characters*/
      drequest->service = service;
      return;
    }
    ctx->set_error(ctx,404,"demo service \"%s\" not recognised or not enabled",cpathinfo);
  }
}

void _create_demo_front(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                        const char *urlprefix)
{
  int i;
  char *caps;
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head_title,"mapcache demo landing page");
  for(i=0; i<MAPCACHE_SERVICES_COUNT; i++) {
    mapcache_service *service = ctx->config->services[i];
    if(!service || service->type == MAPCACHE_SERVICE_DEMO) continue; /* skip an unconfigured service, and the demo one */
    caps = apr_pstrcat(ctx->pool,caps,"<a href=\"",urlprefix,"demo/",service->name,"\">",
                       service->name,"</a><br/>\n",NULL);
  }
  caps = apr_pstrcat(ctx->pool,caps,demo_footer_title,NULL);

  req->capabilities = caps;
}

void _create_demo_wms(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                      const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;
  mapcache_service_wms *service = (mapcache_service_wms*)ctx->config->services[MAPCACHE_SERVICE_WMS];
#ifdef DEBUG
  if(!service) {
    ctx->set_error(ctx,500,"##BUG## wms service disabled in demo");
    return;
  }
#endif
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head, "");
  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    int i,j;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    for(j=0; j<tileset->grid_links->nelts; j++) {
      char *resolutions="";
      char *unit="dd";
      char *smerc = "false";
      char *ol_layer_name;
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(grid->unit == MAPCACHE_UNIT_METERS) {
        unit="m";
      } else if(grid->unit == MAPCACHE_UNIT_FEET) {
        unit="ft";
      }
      if(strstr(grid->srs, ":900913") || strstr(grid->srs, ":3857")) {
        smerc = "true";
      }

      resolutions = apr_psprintf(ctx->pool,"%s%.20f",resolutions,grid->levels[grid_link->minz]->resolution);
      for(i=grid_link->minz+1; i<grid_link->maxz; i++) {
        resolutions = apr_psprintf(ctx->pool,"%s,%.20f",resolutions,grid->levels[i]->resolution);
      }

      if(!tileset->timedimension) {
        ol_layer_name = apr_psprintf(ctx->pool, "%s_%s", tileset->name, grid->name);
        /* normalize name to something that is a valid variable name */
        for(i=0; i<strlen(ol_layer_name); i++)
          if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
              || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
            ol_layer_name[i] = '_';

        ol_layer = apr_psprintf(ctx->pool,demo_layer_wms,
                                ol_layer_name,
                                tileset->name,
                                grid->name,
                                apr_pstrcat(ctx->pool,url_prefix,"?",NULL),
                                tileset->name,
                                resolutions,
                                unit,
                                grid->extent.minx,
                                grid->extent.miny,
                                grid->extent.maxx,
                                grid->extent.maxy,
                                grid->srs,
                                smerc,
                                ol_layer_name);
        caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);

        if(service->getmap_strategy == MAPCACHE_GETMAP_ASSEMBLE) {
          ol_layer = apr_psprintf(ctx->pool,demo_layer_singletile,
                                  ol_layer_name,
                                  tileset->name,
                                  grid->name,
                                  apr_pstrcat(ctx->pool,url_prefix,"?",NULL),
                                  tileset->name,resolutions,unit,
                                  grid->extent.minx,
                                  grid->extent.miny,
                                  grid->extent.maxx,
                                  grid->extent.maxy,
                                  grid->srs,
                                  smerc,
                                  ol_layer_name);
          caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
        }
      } else {
        int id;
        apr_array_header_t *timedimvals = tileset->timedimension->get_all_entries(
                ctx,tileset->timedimension,tileset);
        for(id=0;id<timedimvals->nelts;id++) {
          if(id>1) break;
          char *idval = APR_ARRAY_IDX(timedimvals,id,char*);
          char *dimparam_wms = "    %s_wms_layer.mergeNewParams({%s:\"%s\"});\n";
          char *dimparam_singletile = "    %s_slayer.mergeNewParams({%s:\"%s\"});\n";
          ol_layer_name = apr_psprintf(ctx->pool, "%s_%s_%s", tileset->name, grid->name, idval);
          /* normalize name to something that is a valid variable name */
          for(i=0; i<strlen(ol_layer_name); i++)
            if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
                || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
              ol_layer_name[i] = '_';
          ol_layer = apr_psprintf(ctx->pool, demo_layer_wms,
                                  ol_layer_name,
                                  tileset->name,
                                  grid->name,
                                  apr_pstrcat(ctx->pool,url_prefix,"?",NULL),
                                  tileset->name,
                                  resolutions,
                                  unit,
                                  grid->extent.minx,
                                  grid->extent.miny,
                                  grid->extent.maxx,
                                  grid->extent.maxy,
                                  grid->srs,
                                  smerc,
                                  ol_layer_name);
          caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
          caps = apr_psprintf(ctx->pool,"%s%s",caps,
                  apr_psprintf(ctx->pool,dimparam_wms,ol_layer_name,tileset->timedimension->key,idval));
            
          if(service->getmap_strategy == MAPCACHE_GETMAP_ASSEMBLE) {
            ol_layer = apr_psprintf(ctx->pool, demo_layer_singletile,
                                    ol_layer_name,
                                    tileset->name,
                                    grid->name,
                                    apr_pstrcat(ctx->pool,url_prefix,"?",NULL),
                                    tileset->name,
                                    resolutions,
                                    unit,
                                    grid->extent.minx,
                                    grid->extent.miny,
                                    grid->extent.maxx,
                                    grid->extent.maxy,
                                    grid->srs,
                                    smerc,
                                    ol_layer_name);
            caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
            caps = apr_psprintf(ctx->pool,"%s%s",caps,
                    apr_psprintf(ctx->pool,dimparam_singletile,ol_layer_name,tileset->timedimension->key,idval));
          }
        }
      }
    }
    if(tileset->source && tileset->source->info_formats) {
      char *ol_layer_name;

      ol_layer_name = apr_psprintf(ctx->pool, "%s", tileset->name);
      /* normalize name to something that is a valid variable name */
      for(i=0; i<strlen(ol_layer_name); i++)
        if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
            || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
          ol_layer_name[i] = '_';

      ol_layer = apr_psprintf(ctx->pool, demo_control_featureinfo,
                              ol_layer_name,
                              apr_pstrcat(ctx->pool,url_prefix,"?",NULL),
                              APR_ARRAY_IDX(tileset->source->info_formats,0,char*),
                              ol_layer_name,
                              ol_layer_name);
      caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer,caps);

  req->capabilities = caps;
}

static char *demo_layer_mapguide =
  "    var %s_mg_layer = new OpenLayers.Layer.MapGuide( \"%s-%s-MapGuide\",\n"
  "        \"%s\",\n"
  "        { basemaplayergroupname: '%s@%s', format:'png' },\n"
  "        { gutter:0,buffer:0,isBaseLayer:true,transitionEffect:'resize',\n"
  "          resolutions:[%s],\n"
  "          units:\"%s\",\n"
  "          useHttpTile:true,\n"
  "          maxExtent: new OpenLayers.Bounds(%f,%f,%f,%f),\n"
  "          projection: new OpenLayers.Projection(\"%s\".toUpperCase()),\n"
  "          singleTile: false,\n"
  "          sphericalMercator: %s,\n"
	"          defaultSize: new OpenLayers.Size(%d,%d)\n"
  "        }\n"
  "    );\n"
  "    map.addLayer(%s_mg_layer);\n\n";

void _create_demo_mapguide(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                      const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;

  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head, "");

  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    int i,j;
    char *extension;
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    extension = "png";
    if (tileset->format && tileset->format->extension)
      extension = tileset->format->extension;
    for(j=0; j<tileset->grid_links->nelts; j++) {
      char *resolutions="";
      char *unit="dd";
      char *smerc = "false";
      char *ol_layer_name;
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(grid->unit == MAPCACHE_UNIT_METERS) {
        unit="m";
      } else if(grid->unit == MAPCACHE_UNIT_FEET) {
        unit="ft";
      }
      if(strstr(grid->srs, ":900913") || strstr(grid->srs, ":3857")) {
        smerc = "true";
      }
      ol_layer_name = apr_psprintf(ctx->pool, "%s_%s", tileset->name, grid->name);
      /* normalize name to something that is a valid variable name */
      for(i=0; i<strlen(ol_layer_name); i++)
        if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
            || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
          ol_layer_name[i] = '_';

      resolutions = apr_psprintf(ctx->pool,"%s%.20f",resolutions,grid->levels[grid_link->minz]->resolution);
      for(i=grid_link->minz+1; i<grid_link->maxz; i++) {
        resolutions = apr_psprintf(ctx->pool,"%s,%.20f",resolutions,grid->levels[i]->resolution);
      }

      ol_layer = apr_psprintf(ctx->pool, demo_layer_mapguide,
                              ol_layer_name,
                              tileset->name,
                              grid->name,
                              apr_pstrcat(ctx->pool,url_prefix,"mg/",NULL),
                              tileset->name,
                              grid->name,
                              resolutions,
                              unit,
                              grid->extent.minx,
                              grid->extent.miny,
                              grid->extent.maxx,
                              grid->extent.maxy,
                              grid->srs,
                              smerc,
                              grid->tile_sx,grid->tile_sy,
                              ol_layer_name);
      caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer,caps);

  req->capabilities = caps;
}
void _create_demo_tms(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                      const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;

  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head, "");

  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    int i,j;
    char *extension;
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    extension = "png";
    if (tileset->format && tileset->format->extension)
      extension = tileset->format->extension;
    for(j=0; j<tileset->grid_links->nelts; j++) {
      char *resolutions="";
      char *unit="dd";
      char *smerc = "false";
      char *ol_layer_name;
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(grid->unit == MAPCACHE_UNIT_METERS) {
        unit="m";
      } else if(grid->unit == MAPCACHE_UNIT_FEET) {
        unit="ft";
      }
      if(strstr(grid->srs, ":900913") || strstr(grid->srs, ":3857")) {
        smerc = "true";
      }
      ol_layer_name = apr_psprintf(ctx->pool, "%s_%s", tileset->name, grid->name);
      /* normalize name to something that is a valid variable name */
      for(i=0; i<strlen(ol_layer_name); i++)
        if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
            || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
          ol_layer_name[i] = '_';

      resolutions = apr_psprintf(ctx->pool,"%s%.20f",resolutions,grid->levels[grid_link->minz]->resolution);
      for(i=grid_link->minz+1; i<grid_link->maxz; i++) {
        resolutions = apr_psprintf(ctx->pool,"%s,%.20f",resolutions,grid->levels[i]->resolution);
      }

      ol_layer = apr_psprintf(ctx->pool, demo_layer_tms,
                              ol_layer_name,
                              tileset->name,
                              grid->name,
                              apr_pstrcat(ctx->pool,url_prefix,"tms/",NULL),
                              tileset->name,
                              grid->name,
                              extension,
                              grid->extent.minx,
                              grid->extent.miny,
                              resolutions,
                              grid_link->minz,
                              unit,
                              grid->extent.minx,
                              grid->extent.miny,
                              grid->extent.maxx,
                              grid->extent.maxy,
                              grid->srs,
                              smerc,
                              ol_layer_name);
      caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer,caps);

  req->capabilities = caps;
}

void _create_demo_wmts(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                       const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head, "");

  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    int i,j;
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    char *mime_type;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    mime_type = "image/png";
    if (tileset->format && tileset->format->mime_type)
      mime_type = tileset->format->mime_type;
    for(j=0; j<tileset->grid_links->nelts; j++) {
      char *resolutions="";
      char *unit="dd";
      char *smerc = "false";
      char *ol_layer_name;
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(grid->unit == MAPCACHE_UNIT_METERS) {
        unit="m";
      } else if(grid->unit == MAPCACHE_UNIT_FEET) {
        unit="ft";
      }
      if(strstr(grid->srs, ":900913") || strstr(grid->srs, ":3857")) {
        smerc = "true";
      }

      resolutions = apr_psprintf(ctx->pool,"%s%.20f",resolutions,grid->levels[grid_link->minz]->resolution);
      for(i=grid_link->minz+1; i<grid_link->maxz; i++) {
        resolutions = apr_psprintf(ctx->pool,"%s,%.20f",resolutions,grid->levels[i]->resolution);
      }

      if(!tileset->timedimension) {
        ol_layer_name = apr_psprintf(ctx->pool, "%s_%s", tileset->name, grid->name);
        /* normalize name to something that is a valid variable name */
        for(i=0; i<strlen(ol_layer_name); i++)
          if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
              || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
            ol_layer_name[i] = '_';
        ol_layer = apr_psprintf(ctx->pool, demo_layer_wmts,
                                ol_layer_name,
                                tileset->name,
                                grid->name,
                                apr_pstrcat(ctx->pool,url_prefix,"wmts/",NULL),
                                tileset->name,
                                grid->name,
                                mime_type,
                                resolutions,
                                grid_link->minz,
                                unit,
                                grid->extent.minx,
                                grid->extent.miny,
                                grid->extent.maxx,
                                grid->extent.maxy,
                                grid->srs,
                                smerc,
                                ol_layer_name);
        caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
      } else {
        int id;
        apr_array_header_t *timedimvals = tileset->timedimension->get_all_entries(
                ctx,tileset->timedimension,tileset);
        GC_CHECK_ERROR(ctx);
        for(id=0;id<timedimvals->nelts;id++) {
          if(id>1) break; /* we don't want all the entries in the demo interface */
          char *idval = APR_ARRAY_IDX(timedimvals,id,char*);
          char *dimparam = "%s_wmts_layer.mergeNewParams({%s:\"%s\"});\n";
          ol_layer_name = apr_psprintf(ctx->pool, "%s_%s_%s", tileset->name, grid->name, idval);
          /* normalize name to something that is a valid variable name */
          for(i=0; i<strlen(ol_layer_name); i++)
            if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
                || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
              ol_layer_name[i] = '_';
          ol_layer = apr_psprintf(ctx->pool, demo_layer_wmts,
                                ol_layer_name,
                                tileset->name,
                                grid->name,
                                apr_pstrcat(ctx->pool,url_prefix,"wmts/",NULL),
                                tileset->name,
                                grid->name,
                                mime_type,
                                resolutions,
                                grid_link->minz,
                                unit,
                                grid->extent.minx,
                                grid->extent.miny,
                                grid->extent.maxx,
                                grid->extent.maxy,
                                grid->srs,
                                smerc,
                                ol_layer_name);
          caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
          caps = apr_psprintf(ctx->pool,"%s%s",caps,
                  apr_psprintf(ctx->pool,dimparam,ol_layer_name,tileset->timedimension->key,idval));
            
        }
      }
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer,caps);

  req->capabilities = caps;
}

void _create_demo_ve(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                     const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head, demo_ve_extra);

  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    int i,j;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    for(j=0; j<tileset->grid_links->nelts; j++) {
      char *resolutions="";
      char *unit="dd";
      char *smerc = "false";
      char *ol_layer_name;
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(grid->unit == MAPCACHE_UNIT_METERS) {
        unit="m";
      } else if(grid->unit == MAPCACHE_UNIT_FEET) {
        unit="ft";
      }
      if(strstr(grid->srs, ":900913") || strstr(grid->srs, ":3857")) {
        smerc = "true";
      }
      ol_layer_name = apr_psprintf(ctx->pool, "%s_%s", tileset->name, grid->name);
      /* normalize name to something that is a valid variable name */
      for(i=0; i<strlen(ol_layer_name); i++)
        if ((!i && !isalpha(ol_layer_name[i]) && ol_layer_name[i] != '_')
            || (!isalnum(ol_layer_name[i]) && ol_layer_name[i] != '_'))
          ol_layer_name[i] = '_';

      resolutions = apr_psprintf(ctx->pool,"%s%.20f",resolutions,grid->levels[grid_link->minz]->resolution);
      for(i=grid_link->minz+1; i<grid_link->maxz; i++) {
        resolutions = apr_psprintf(ctx->pool,"%s,%.20f",resolutions,grid->levels[i]->resolution);
      }

      ol_layer = apr_psprintf(ctx->pool, demo_layer_ve,
                              ol_layer_name,
                              tileset->name,
                              grid->name,
                              apr_pstrcat(ctx->pool,url_prefix,"ve",NULL),
                              tileset->name,
                              grid->name,
                              resolutions,
                              unit,
                              grid->extent.minx,
                              grid->extent.miny,
                              grid->extent.maxx,
                              grid->extent.maxy,
                              grid->srs,
                              smerc,
                              ol_layer_name);
      caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer,caps);

  req->capabilities = caps;
}

void _create_demo_kml(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                      const char *url_prefix)
{
  char *caps;
  apr_hash_index_t *tileindex_index;
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_psprintf(ctx->pool,demo_head_title,"mapcache kml links");
  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    mapcache_tileset *tileset;
    int j;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    for(j=0; j<tileset->grid_links->nelts; j++) {
      mapcache_grid *grid = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*)->grid;
      if(!strstr(grid->srs, ":4326")) {
        continue; //skip layers not in wgs84
      }
      caps = apr_pstrcat(ctx->pool,caps,
                         "<li><a href=\"",url_prefix,"kml/",tileset->name,
                         "@",grid->name,".kml\">",tileset->name,"</a></li>\n",
                         NULL);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }

  caps = apr_pstrcat(ctx->pool,caps,demo_footer_title,NULL);
  req->capabilities = caps;
}


void _create_demo_gmaps(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                        const char *url_prefix)
{
  char *caps;
  char *ol_layer;
  apr_hash_index_t *tileindex_index;
  req->mime_type = apr_pstrdup(ctx->pool,"text/html");
  caps = apr_pstrdup(ctx->pool,demo_head_gmaps);

  tileindex_index = apr_hash_first(ctx->pool,ctx->config->tilesets);
  while(tileindex_index) {
    mapcache_tileset *tileset;
    int j;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);

    for(j=0; j<tileset->grid_links->nelts; j++) {
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,j,mapcache_grid_link*);
      mapcache_grid *grid = grid_link->grid;
      if(!strstr(grid->srs, ":900913") && !strstr(grid->srs, ":3857"))
        continue; /* skip layers that are not in google projrction */

      ol_layer = apr_psprintf(ctx->pool, demo_layer_gmaps,
                              tileset->name,
                              grid->name,
                              url_prefix,
                              tileset->name,
                              grid->name,
                              grid->tile_sx,
                              grid->tile_sy,
                              tileset->format->extension,
                              grid_link->minz,
                              grid_link->maxz
                             );
      caps = apr_psprintf(ctx->pool,"%s%s",caps,ol_layer);
    }
    tileindex_index = apr_hash_next(tileindex_index);
  }
  caps = apr_psprintf(ctx->pool,demo_footer_gmaps,caps);

  req->capabilities = caps;
}

void _create_capabilities_demo(mapcache_context *ctx, mapcache_request_get_capabilities *req,
                               char *url, char *path_info, mapcache_cfg *cfg)
{
  mapcache_request_get_capabilities_demo *request = (mapcache_request_get_capabilities_demo*)req;
  const char *onlineresource = apr_table_get(cfg->metadata,"url");
  if(!onlineresource) {
    onlineresource = url;
  }

  if(!request->service) {
    return _create_demo_front(ctx,req,onlineresource);
  } else {
    switch(request->service->type) {
      case MAPCACHE_SERVICE_WMS:
        return _create_demo_wms(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_TMS:
        return _create_demo_tms(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_WMTS:
        return _create_demo_wmts(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_VE:
        return _create_demo_ve(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_GMAPS:
        return _create_demo_gmaps(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_KML:
        return _create_demo_kml(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_MAPGUIDE:
        return _create_demo_mapguide(ctx,req,onlineresource);
      case MAPCACHE_SERVICE_DEMO:
        ctx->set_error(ctx,400,"selected service does not provide a demo page");
        return;
    }
  }



}

mapcache_service* mapcache_service_demo_create(mapcache_context *ctx)
{
  mapcache_service_demo* service = (mapcache_service_demo*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_demo));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate demo service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool,"demo");
  service->service.name = apr_pstrdup(ctx->pool,"demo");
  service->service.type = MAPCACHE_SERVICE_DEMO;
  service->service.parse_request = _mapcache_service_demo_parse_request;
  service->service.create_capabilities_response = _create_capabilities_demo;
  return (mapcache_service*)service;
}

/** @} *//* vim: ts=2 sts=2 et sw=2
*/
