/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: WMTS service
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
#include <apr_tables.h>
#include "mapcache_services.h"

/** \addtogroup services */
/** @{ */


static ezxml_t _wmts_capabilities(mapcache_context *ctx, mapcache_cfg *cfg)
{
  char *schemaLocation = "http://www.opengis.net/wmts/1.0 http://schemas.opengis.net/wmts/1.0/wmtsGetCapabilities_response.xsd";

  ezxml_t node = ezxml_new("Capabilities");
  ezxml_set_attr(node,"xmlns","http://www.opengis.net/wmts/1.0");
  ezxml_set_attr(node,"xmlns:ows","http://www.opengis.net/ows/1.1");
  ezxml_set_attr(node,"xmlns:xlink","http://www.w3.org/1999/xlink");
  ezxml_set_attr(node,"xmlns:xsi","http://www.w3.org/2001/XMLSchema-instance");
  ezxml_set_attr(node,"xmlns:gml","http://www.opengis.net/gml");

  if( apr_table_get(cfg->metadata,"inspire_profile") ) {
    ezxml_set_attr(node,"xmlns:inspire_common","http://inspire.ec.europa.eu/schemas/common/1.0");
    ezxml_set_attr(node,"xmlns:inspire_vs","http://inspire.ec.europa.eu/schemas/inspire_vs_ows11/1.0");
    schemaLocation = apr_pstrcat(ctx->pool,schemaLocation," http://inspire.ec.europa.eu/schemas/inspire_vs_ows11/1.0 http://inspire.ec.europa.eu/schemas/inspire_vs_ows11/1.0/inspire_vs_ows_11.xsd",NULL);
  }

  ezxml_set_attr(node,"xsi:schemaLocation",schemaLocation);
  ezxml_set_attr(node,"version","1.0.0");

  return node;
}

int _wmts_service_identification_keywords(void *in, const char *key, const char *value) {
   ezxml_t node = (ezxml_t )in;
   if (!strcasecmp(key,"keyword")) {
     ezxml_set_txt(ezxml_add_child(node,"ows:Keyword",0),value);
   } else {
     ezxml_set_txt(ezxml_add_child(node,key,0),value);
   }

   return 1;
}

static ezxml_t _wmts_service_identification(mapcache_context *ctx, mapcache_cfg *cfg)
{
  const char *value;

  ezxml_t node = ezxml_new("ows:ServiceIdentification");

  value = apr_table_get(cfg->metadata,"title");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:Title",0),value);

  value = apr_table_get(cfg->metadata,"abstract");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:Abstract",0),value);

  value = apr_table_get(cfg->metadata,"keyword");
  if(value) {
    ezxml_t nodeKeywords = ezxml_new("ows:Keywords");
    apr_table_do(_wmts_service_identification_keywords, nodeKeywords, cfg->metadata, "keyword", NULL);
    ezxml_insert(nodeKeywords, node, 0);
  }

  ezxml_set_txt(ezxml_add_child(node,"ows:ServiceType",0),"OGC WMTS");
  ezxml_set_txt(ezxml_add_child(node,"ows:ServiceTypeVersion",0),"1.0.0");

  value = apr_table_get(cfg->metadata,"fees");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:Fees",0),value);

  value = apr_table_get(cfg->metadata,"accessconstraints");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:AccessConstraints",0),value);

  return node;
}

int _wmts_inspire_metadata_responselanguages(void *in, const char *key, const char *value) {
   ezxml_t node = (ezxml_t )in;
   ezxml_set_txt(ezxml_add_child(node,"inspire_common:Language",0),value);

   return 1;
}

static ezxml_t _wmts_inspire_metadata(mapcache_context *ctx, mapcache_cfg *cfg)
{
  ezxml_t extended = ezxml_new("inspire_vs:ExtendedCapabilities");
  ezxml_t metadata;
  ezxml_t metadataurl;
  ezxml_t metadatatype;
  ezxml_t langsupported;
  ezxml_t langsupporteddefault;
  ezxml_t langsupporteddefaultlang;
  ezxml_t langresponse;
  const char *value;

  metadata = ezxml_add_child(extended,"inspire_common:MetadataUrl",0);
  ezxml_set_attr(metadata,"xsi:type","inspire_common:resourceLocatorType");
  metadataurl = ezxml_add_child(metadata, "inspire_common:URL", 0);

  value = apr_table_get(cfg->metadata, "inspire_metadataurl");
  if(value)
    ezxml_set_txt(metadataurl, value);
 
  metadatatype = ezxml_add_child(metadata, "inspire_common:MediaType", 0);
  ezxml_set_txt(metadatatype, "application/vnd.iso.19139+xml");

  langsupported = ezxml_add_child(extended, "inspire_common:SupportedLanguages", 0);
  langsupporteddefault = ezxml_add_child(langsupported, "inspire_common:DefaultLanguage", 0);
  langsupporteddefaultlang = ezxml_add_child(langsupporteddefault, "inspire_common:Language", 0);

  value = apr_table_get(cfg->metadata, "defaultlanguage");
  if(value)
    ezxml_set_txt(langsupporteddefaultlang, apr_table_get(cfg->metadata, "defaultlanguage"));

  langresponse = ezxml_add_child(extended, "inspire_common:ResponseLanguage", 0);
  apr_table_do(_wmts_inspire_metadata_responselanguages, langresponse, cfg->metadata, "language", NULL);

  return extended;
}

static ezxml_t _wmts_operations_metadata(mapcache_context *ctx, const char *onlineresource, const char *operationstr)
{
  ezxml_t http;
  ezxml_t dcp;
  ezxml_t get;
  ezxml_t constraint;
  ezxml_t allowedvalues;
  ezxml_t operation = ezxml_new("ows:Operation");
  ezxml_set_attr(operation,"name",operationstr);
  dcp = ezxml_add_child(operation,"ows:DCP",0);
  http = ezxml_add_child(dcp,"ows:HTTP",0);
  get = ezxml_add_child(http,"ows:Get",0);
  ezxml_set_attr(get,"xlink:href",apr_pstrcat(ctx->pool,onlineresource,"wmts?",NULL));
  constraint = ezxml_add_child(get,"ows:Constraint",0);
  ezxml_set_attr(constraint,"name","GetEncoding");
  allowedvalues = ezxml_add_child(constraint,"ows:AllowedValues",0);
  ezxml_set_txt(ezxml_add_child(allowedvalues,"ows:Value",0),"KVP");
  return operation;

}

static ezxml_t _wmts_service_contactinfo(mapcache_context *ctx, mapcache_cfg *cfg)
{
  const char *value;
  int addNode = 0;
  ezxml_t nodeAddress;

  ezxml_t nodeInfo = ezxml_new("ows:ContactInfo");
  ezxml_t nodePhone = ezxml_new("ows:Phone");

  value = apr_table_get(cfg->metadata,"contactphone"); 
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodePhone,"ows:Voice",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactfacsimile"); 
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodePhone,"ows:Facsimile",0),value);
  }

  if( addNode == 1 )
    ezxml_insert(nodePhone, nodeInfo, 0);


  addNode = 0;
  nodeAddress = ezxml_new("ows:Address");
  
  value = apr_table_get(cfg->metadata,"contactorganization");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:DeliveryPoint",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactcity");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:City",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactstateorprovince");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:AdministrativeArea",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactpostcode");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:PostalCode",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactcountry");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:Country",0),value);
  }

  value = apr_table_get(cfg->metadata,"contactelectronicmailaddress");
  if(value) {
    addNode = 1;
    ezxml_set_txt(ezxml_add_child(nodeAddress,"ows:ElectronicMailAddress",0),value);
  }

  if( addNode == 1 )
    ezxml_insert(nodeAddress, nodeInfo, 0);

  return nodeInfo;
}

static ezxml_t _wmts_service_contact(mapcache_context *ctx, mapcache_cfg *cfg)
{
  const char *value;

  ezxml_t node = ezxml_new("ows:ServiceContact");

  value = apr_table_get(cfg->metadata,"contactname");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:IndividualName",0),value);

  value = apr_table_get(cfg->metadata,"contactposition");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:PositionName",0),value);

  ezxml_insert(_wmts_service_contactinfo(ctx,cfg),node,0);

  return node;
}

static ezxml_t _wmts_service_provider(mapcache_context *ctx, mapcache_cfg *cfg)
{
  const char *value;

  ezxml_t node = ezxml_new("ows:ServiceProvider");

  value = apr_table_get(cfg->metadata,"providername");
  if(value)
    ezxml_set_txt(ezxml_add_child(node,"ows:ProviderName",0),value);

  value = apr_table_get(cfg->metadata,"providerurl");
  if(value)
    ezxml_set_attr(ezxml_add_child(node,"ows:ProviderSite",0),"xlink:href",value);

  ezxml_insert(_wmts_service_contact(ctx,cfg),node,0);

  return node;
}


void _create_capabilities_wmts(mapcache_context *ctx, mapcache_request_get_capabilities *req, char *url, char *path_info, mapcache_cfg *cfg)
{
  const char *onlineresource;
  mapcache_request_get_capabilities_wmts *request = (mapcache_request_get_capabilities_wmts*)req;
  ezxml_t caps;
  ezxml_t contents;
  ezxml_t operations_metadata;
  apr_hash_index_t *layer_index;
  apr_hash_index_t *grid_index;
  char *tmpcaps;
  apr_table_t *requiredGrids = apr_table_make(ctx->pool, apr_hash_count(cfg->grids));
#ifdef DEBUG
  if(request->request.request.type != MAPCACHE_REQUEST_GET_CAPABILITIES) {
    ctx->set_error(ctx,500,"wrong wmts capabilities request");
    return;
  }
#endif

  onlineresource = apr_table_get(cfg->metadata,"url");
  if(!onlineresource) {
    onlineresource = url;
  }

  request->request.mime_type = apr_pstrdup(ctx->pool,"application/xml");

  caps = _wmts_capabilities(ctx,cfg);
  ezxml_insert(_wmts_service_identification(ctx,cfg),caps,0);
  ezxml_insert(_wmts_service_provider(ctx,cfg),caps,0);

  operations_metadata = ezxml_add_child(caps,"ows:OperationsMetadata",0);
  ezxml_insert(_wmts_operations_metadata(ctx,onlineresource,"GetCapabilities"),operations_metadata,0);
  ezxml_insert(_wmts_operations_metadata(ctx,onlineresource,"GetTile"),operations_metadata,0);
  ezxml_insert(_wmts_operations_metadata(ctx,onlineresource,"GetFeatureInfo"),operations_metadata,0);

  /* @todo: support both, url and embed profile */
  if( apr_table_get(cfg->metadata,"inspire_profile") )
    ezxml_insert(_wmts_inspire_metadata(ctx,cfg),operations_metadata,0);

  contents = ezxml_add_child(caps,"Contents",0);

  layer_index = apr_hash_first(ctx->pool,cfg->tilesets);
  while(layer_index) {
    int i;
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    ezxml_t layer;
    const char *title;
    const char *abstract;
    const char *keywords;
    ezxml_t style;
    char *dimensionstemplate="";
    ezxml_t resourceurl;

    apr_hash_this(layer_index,&key,&keylen,(void**)&tileset);

    layer = ezxml_add_child(contents,"Layer",0);

    /* optional layer title */
    title = apr_table_get(tileset->metadata,"title");
    if(title) {
      ezxml_set_txt(ezxml_add_child(layer,"ows:Title",0),title);
    } else {
      ezxml_set_txt(ezxml_add_child(layer,"ows:Title",0),tileset->name);

    /* optional layer abstract */
    }
    abstract = apr_table_get(tileset->metadata,"abstract");
    if(abstract) {
      ezxml_set_txt(ezxml_add_child(layer,"ows:Abstract",0),abstract);
    }

    // optional layer keywords
    // `>` suffix in name indicates that a table is expected instead of a string
    // (see `parseMetadata()` in `configuration_xml.c`)
    keywords = apr_table_get(tileset->metadata,"keywords>");
    if (keywords) {
      apr_table_t * contents = (apr_table_t *)keywords;
      keywords = apr_table_get(contents,"keyword");
      if (keywords) {
        ezxml_t nodeKeywords = ezxml_new("ows:Keywords");
        apr_table_do(_wmts_service_identification_keywords, nodeKeywords, contents, "keyword", NULL);
        ezxml_insert(nodeKeywords, layer, 0);
      }
    }

    if(tileset->wgs84bbox.minx != tileset->wgs84bbox.maxx) {
      ezxml_t bbox = ezxml_add_child(layer,"ows:WGS84BoundingBox",0);
      ezxml_set_txt(ezxml_add_child(bbox,"ows:LowerCorner",0),
                    apr_psprintf(ctx->pool,"%f %f",tileset->wgs84bbox.minx, tileset->wgs84bbox.miny));
      ezxml_set_txt(ezxml_add_child(bbox,"ows:UpperCorner",0),
                    apr_psprintf(ctx->pool,"%f %f",tileset->wgs84bbox.maxx, tileset->wgs84bbox.maxy));
    }


    ezxml_set_txt(ezxml_add_child(layer,"ows:Identifier",0),tileset->name);

    style = ezxml_add_child(layer,"Style",0);
    ezxml_set_attr(style,"isDefault","true");
    ezxml_set_txt(ezxml_add_child(style,"ows:Identifier",0),"default");

    if(tileset->format && tileset->format->mime_type)
      ezxml_set_txt(ezxml_add_child(layer,"Format",0),tileset->format->mime_type);
    else
      ezxml_set_txt(ezxml_add_child(layer,"Format",0),"image/unknown");



    if(tileset->dimensions) {
      for(i=0; i<tileset->dimensions->nelts; i++) {
        apr_array_header_t *values;
        int value_idx;
        mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
        ezxml_t dim = ezxml_add_child(layer,"Dimension",0);
        ezxml_set_txt(ezxml_add_child(dim,"ows:Identifier",0),dimension->name);
        ezxml_set_txt(ezxml_add_child(dim,"Default",0),dimension->default_value);

        if(dimension->unit) {
          ezxml_set_txt(ezxml_add_child(dim,"UOM",0),dimension->unit);
        }
        values = dimension->get_all_ogc_formatted_entries(ctx,dimension,tileset,NULL,NULL);
        for(value_idx=0;value_idx<values->nelts;value_idx++) {
          char *idval = APR_ARRAY_IDX(values,value_idx,char*);
          ezxml_set_txt(ezxml_add_child(dim,"Value",0),idval);
        }
        dimensionstemplate = apr_pstrcat(ctx->pool,dimensionstemplate,"{",dimension->name,"}/",NULL);
      }
    }
    if(tileset->source && tileset->source->info_formats) {
      int i;
      for(i=0; i<tileset->source->info_formats->nelts; i++) {
        char *iformat = APR_ARRAY_IDX(tileset->source->info_formats,i,char*);
        ezxml_set_txt(ezxml_add_child(layer,"InfoFormat",0),iformat);
      }
    }


    for(i=0; i<tileset->grid_links->nelts; i++) {
      mapcache_grid_link *grid_link = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
      ezxml_t tmsetlnk = ezxml_add_child(layer,"TileMatrixSetLink",0);
      ezxml_set_txt(ezxml_add_child(tmsetlnk,"TileMatrixSet",0),grid_link->grid->name);

      /*
       * remember TileMatrixSetLinks
       */
      apr_table_setn(requiredGrids, grid_link->grid->name, "true");

      if(grid_link->restricted_extent) {
        ezxml_t limits = ezxml_add_child(tmsetlnk,"TileMatrixSetLimits",0);
        int j;
        for(j=0; j<grid_link->grid->nlevels; j++) {
          ezxml_t matrixlimits = ezxml_add_child(limits,"TileMatrixLimits",0);
          ezxml_set_txt(ezxml_add_child(matrixlimits,"TileMatrix",0),
                        apr_psprintf(ctx->pool,"%s:%d",grid_link->grid->name,j));
          ezxml_set_txt(ezxml_add_child(matrixlimits,"MinTileRow",0),
                        apr_psprintf(ctx->pool,"%d",grid_link->grid_limits[j].miny));
          ezxml_set_txt(ezxml_add_child(matrixlimits,"MaxTileRow",0),
                        apr_psprintf(ctx->pool,"%d",grid_link->grid_limits[j].maxy-1));
          ezxml_set_txt(ezxml_add_child(matrixlimits,"MinTileCol",0),
                        apr_psprintf(ctx->pool,"%d",grid_link->grid_limits[j].minx));
          ezxml_set_txt(ezxml_add_child(matrixlimits,"MaxTileCol",0),
                        apr_psprintf(ctx->pool,"%d",grid_link->grid_limits[j].maxx-1));
        }
      }

      /*
       * gaia gis chokes if this is added to the capabilities doc,
       * so disable it for now
       *
      double *gbbox = grid_link->restricted_extent?grid_link->restricted_extent:grid_link->grid->extent;
      ezxml_t bbox = ezxml_add_child(layer,"ows:BoundingBox",0);
      ezxml_set_txt(ezxml_add_child(bbox,"ows:LowerCorner",0),
            apr_psprintf(ctx->pool,"%f %f",gbbox[0], gbbox[1]));
      ezxml_set_txt(ezxml_add_child(bbox,"ows:UpperCorner",0),
            apr_psprintf(ctx->pool,"%f %f",gbbox[2], gbbox[3]));
      ezxml_set_attr(bbox,"crs",mapcache_grid_get_crs(ctx,grid_link->grid));
      */
    }

    if(tileset->source && tileset->source->info_formats) {
      int i;
      for(i=0; i<tileset->source->info_formats->nelts; i++) {
        char *iformat = APR_ARRAY_IDX(tileset->source->info_formats,i,char*);
        ezxml_t resourceurl;
        resourceurl = ezxml_add_child(layer,"ResourceURL",0);
        ezxml_set_attr(resourceurl,"format",iformat);
        ezxml_set_attr(resourceurl,"resourceType","FeatureInfo");
        ezxml_set_attr(resourceurl,"template",
                       apr_pstrcat(ctx->pool,onlineresource,"wmts/1.0.0/",tileset->name,"/default/",
                                   dimensionstemplate,"{TileMatrixSet}/{TileMatrix}/{TileRow}/{TileCol}/{J}/{I}.",apr_psprintf(ctx->pool,"%d",i),NULL));
      }
    }

    resourceurl = ezxml_add_child(layer,"ResourceURL",0);
    if(tileset->format && tileset->format->mime_type)
      ezxml_set_attr(resourceurl,"format",tileset->format->mime_type);
    else
      ezxml_set_attr(resourceurl,"format","image/unknown");
    ezxml_set_attr(resourceurl,"resourceType","tile");
    ezxml_set_attr(resourceurl,"template",
                   apr_pstrcat(ctx->pool,onlineresource,"wmts/1.0.0/",tileset->name,"/default/",
                               dimensionstemplate,"{TileMatrixSet}/{TileMatrix}/{TileRow}/{TileCol}.",
                               ((tileset->format)?tileset->format->extension:"xxx"),NULL));

    layer_index = apr_hash_next(layer_index);
  }



  grid_index = apr_hash_first(ctx->pool,cfg->grids);
  while(grid_index) {
    mapcache_grid *grid;
    const void *key;
    apr_ssize_t keylen;
    int level;
    const char *WellKnownScaleSet;
    ezxml_t tmset;
    ezxml_t bbox;
    apr_hash_this(grid_index,&key,&keylen,(void**)&grid);

    /*
     * Skip grids which are not configures at tileset level
     */
    if( !apr_table_get(requiredGrids, grid->name) ) {
      grid_index = apr_hash_next(grid_index);
      continue;
    }

    WellKnownScaleSet = apr_table_get(grid->metadata,"WellKnownScaleSet");

    tmset = ezxml_add_child(contents,"TileMatrixSet",0);
    ezxml_set_txt(ezxml_add_child(tmset,"ows:Identifier",0),grid->name);

    bbox = ezxml_add_child(tmset,"ows:BoundingBox",0);

    if(mapcache_is_axis_inverted(grid->srs)) {
      ezxml_set_txt(ezxml_add_child(bbox,"ows:LowerCorner",0),apr_psprintf(ctx->pool,"%f %f",
                    grid->extent.miny, grid->extent.minx));
      ezxml_set_txt(ezxml_add_child(bbox,"ows:UpperCorner",0),apr_psprintf(ctx->pool,"%f %f",
                    grid->extent.maxy, grid->extent.maxx));
    } else {
      ezxml_set_txt(ezxml_add_child(bbox,"ows:LowerCorner",0),apr_psprintf(ctx->pool,"%f %f",
                    grid->extent.minx, grid->extent.miny));
      ezxml_set_txt(ezxml_add_child(bbox,"ows:UpperCorner",0),apr_psprintf(ctx->pool,"%f %f",
                    grid->extent.maxx, grid->extent.maxy));
    }

    ezxml_set_attr(bbox,"crs",mapcache_grid_get_crs(ctx,grid));

    ezxml_set_txt(ezxml_add_child(tmset,"ows:SupportedCRS",0),mapcache_grid_get_crs(ctx,grid));

    if(WellKnownScaleSet) {
      ezxml_set_txt(ezxml_add_child(tmset,"WellKnownScaleSet",0),WellKnownScaleSet);
    }

    for(level=0; level<grid->nlevels; level++) {
      double scaledenom,tlx,tly;
      mapcache_grid_level *glevel = grid->levels[level];
      ezxml_t tm = ezxml_add_child(tmset,"TileMatrix",0);
      ezxml_set_txt(ezxml_add_child(tm,"ows:Identifier",0),apr_psprintf(ctx->pool,"%d",level));
      scaledenom = glevel->resolution * mapcache_meters_per_unit[grid->unit] / 0.00028;
      ezxml_set_txt(ezxml_add_child(tm,"ScaleDenominator",0),apr_psprintf(ctx->pool,"%.20f",scaledenom));
      switch(grid->origin) {
        case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
          tlx = grid->extent.minx;
          tly = grid->extent.maxy;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
          tlx = grid->extent.minx;
          tly = grid->extent.miny + glevel->maxy * glevel->resolution * grid->tile_sy;
          break;
        case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
        case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
        default:
          ctx->set_error(ctx,500,"origin not implemented");
          return;
      }
      if(mapcache_is_axis_inverted(grid->srs)) {
        ezxml_set_txt(ezxml_add_child(tm,"TopLeftCorner",0),apr_psprintf(ctx->pool,"%f %f",
            tly,tlx));
      } else {
        ezxml_set_txt(ezxml_add_child(tm,"TopLeftCorner",0),apr_psprintf(ctx->pool,"%f %f",
            tlx,tly));
      }
      ezxml_set_txt(ezxml_add_child(tm,"TileWidth",0),apr_psprintf(ctx->pool,"%d",grid->tile_sx));
      ezxml_set_txt(ezxml_add_child(tm,"TileHeight",0),apr_psprintf(ctx->pool,"%d",grid->tile_sy));
      ezxml_set_txt(ezxml_add_child(tm,"MatrixWidth",0),apr_psprintf(ctx->pool,"%d",glevel->maxx));
      ezxml_set_txt(ezxml_add_child(tm,"MatrixHeight",0),apr_psprintf(ctx->pool,"%d",glevel->maxy));
    }
    grid_index = apr_hash_next(grid_index);
  }
  tmpcaps = ezxml_toxml(caps);
  ezxml_free(caps);
  request->request.capabilities = apr_pstrcat(ctx->pool,"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n",tmpcaps,NULL);
  free(tmpcaps);
}

/**
 * \brief parse a WMTS request
 * \private \memberof mapcache_service_wmts
 * \sa mapcache_service::parse_request()
 */
void _mapcache_service_wmts_parse_request(mapcache_context *ctx, mapcache_service *this, mapcache_request **request,
    const char *pathinfo, apr_table_t *params, mapcache_cfg *config)
{
  const char *str, *service = NULL, *style = NULL, *version = NULL, *layer = NULL, *matrixset = NULL,
#ifdef PEDANTIC_WMTS_FORMAT_CHECK
                    *format = NULL,
#endif
                     *matrix = NULL, *tilecol = NULL, *tilerow = NULL, *extension = NULL,
                      *infoformat = NULL, *fi_i = NULL, *fi_j = NULL;
  apr_table_t *dimtable = NULL;
  mapcache_extent *extent = NULL;
  mapcache_tileset *tileset = NULL;
  int row,col,level,x,y;
  int kvp = 0;
  mapcache_grid_link *grid_link;
  char *endptr;
  service = apr_table_get(params,"SERVICE");

  if(service) {
    /*KVP Parsing*/
    kvp = 1;
    if( strcasecmp(service,"wmts") ) {
      ctx->set_error(ctx,400,"received wmts request with invalid service param %s", service);
      ctx->set_exception(ctx,"InvalidParameterValue","service");
      return;
    }
    str = apr_table_get(params,"REQUEST");
    if(!str) {
      ctx->set_error(ctx, 400, "received wmts request with no request");
      ctx->set_exception(ctx,"MissingParameterValue","request");
      return;
    }
    if( ! strcasecmp(str,"getcapabilities") ) {
      mapcache_request_get_capabilities_wmts *req = (mapcache_request_get_capabilities_wmts*)
          apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_capabilities_wmts));
      req->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
      *request = (mapcache_request*)req;
      return;
    } else if( ! strcasecmp(str,"gettile") || ! strcasecmp(str,"getfeatureinfo")) {
      /* extract our wnated parameters, they will be validated later on */
      tilerow = apr_table_get(params,"TILEROW");
      style = apr_table_get(params,"STYLE");
      if(!style || !*style) style = "default";
      tilecol = apr_table_get(params,"TILECOL");
#ifdef PEDANTIC_WMTS_FORMAT_CHECK
      format = apr_table_get(params,"FORMAT");
#endif
      layer = apr_table_get(params,"LAYER");
      if(!layer) { /*we have to validate this now in order to be able to extract dimensions*/
        ctx->set_error(ctx, 400, "received wmts request with no layer");
        ctx->set_exception(ctx,"MissingParameterValue","layer");
        return;
      } else {
        tileset = mapcache_configuration_get_tileset(config,layer);
        if(!tileset) {
          ctx->set_error(ctx, 400, "received wmts request with invalid layer %s",layer);
          ctx->set_exception(ctx,"InvalidParameterValue","layer");
          return;
        }
      }
      matrixset = apr_table_get(params,"TILEMATRIXSET");
      matrix = apr_table_get(params,"TILEMATRIX");
      if(tileset->dimensions) {
        int i;
        dimtable = apr_table_make(ctx->pool,tileset->dimensions->nelts);
        for(i=0; i<tileset->dimensions->nelts; i++) {
          mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
          const char *value;
          if((value = apr_table_get(params,dimension->name)) != NULL) {
            apr_table_set(dimtable,dimension->name,value);
          }
        }
      }
      if(!strcasecmp(str,"getfeatureinfo")) {
        infoformat = apr_table_get(params,"INFOFORMAT");
        fi_i = apr_table_get(params,"I");
        fi_j = apr_table_get(params,"J");
        if(!infoformat || !fi_i || !fi_j) {
          ctx->set_error(ctx, 400, "received wmts featureinfo request with missing infoformat, i or j");
          if(!infoformat)
            ctx->set_exception(ctx,"MissingParameterValue","infoformat");
          if(!fi_i)
            ctx->set_exception(ctx,"MissingParameterValue","i");
          if(!fi_j)
            ctx->set_exception(ctx,"MissingParameterValue","j");
          return;
        }
      }
    } else {
      ctx->set_error(ctx, 501, "received wmts request with invalid request %s",str);
      ctx->set_exception(ctx,"InvalidParameterValue","request");
      return;
    }
  } else {
    char *key, *last;
    for (key = apr_strtok(apr_pstrdup(ctx->pool,pathinfo), "/", &last); key != NULL;
         key = apr_strtok(NULL, "/", &last)) {
      if(!version) {
        version = key;
        if(strcmp(version,"1.0.0")) {
          ctx->set_error(ctx,404, "received wmts request with invalid version \"%s\" (expecting \"1.0.0\")", version);
          return;
        }
        continue;
      }
      if(!layer) {
        if(!strcmp(key,"WMTSCapabilities.xml")) {
          mapcache_request_get_capabilities_wmts *req = (mapcache_request_get_capabilities_wmts*)
              apr_pcalloc(ctx->pool,sizeof(mapcache_request_get_capabilities_wmts));
          req->request.request.type = MAPCACHE_REQUEST_GET_CAPABILITIES;
          *request = (mapcache_request*)req;
          return;
        }
        layer = key;
        tileset = mapcache_configuration_get_tileset(config,layer);
        if(!tileset) {
          ctx->set_error(ctx, 404, "received wmts request with invalid layer %s",layer);
          return;
        }
        continue;
      }
      if(!style) {
        style = key;
        continue;
      }
      if(tileset->dimensions) {
        int i;
        if(!dimtable)
          dimtable = apr_table_make(ctx->pool,tileset->dimensions->nelts);
        i = apr_table_elts(dimtable)->nelts;
        if(i != tileset->dimensions->nelts) {
          /*we still have some dimensions to parse*/
          mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
          apr_table_set(dimtable,dimension->name,key);
          continue;
        }
      }
      if(!matrixset) {
        matrixset = key;
        continue;
      }
      if(!matrix) {
        matrix=key;
        continue;
      }
      if(!tilerow) {
        tilerow = key;
        continue;
      }

      if(!tilecol) {
        /*if we have a get tile request this is the last element of the uri, and it will also contain the file extension*/

        /*split the string at the first '.'*/
        char *charptr = key;
        while(*charptr && *charptr != '.') charptr++;

        if(*charptr == '.') {
          /*replace '.' with '\0' and advance*/
          *charptr++ = '\0';
          tilecol = key;
          extension = charptr;
        } else {
          tilecol = key;
        }
        continue;
      }

      if(!fi_j) {
        fi_j = key;
        continue;
      }

      if(!fi_i) {
        /*split the string at the first '.'*/
        char *charptr = key;
        while(*charptr && *charptr != '.') charptr++;

        /*replace '.' with '\0' and advance*/
        *charptr++ = '\0';
        fi_i = key;
        extension = charptr;
        continue;
      }

      ctx->set_error(ctx,404,"received request with trailing data starting with %s",key);
      return;
    }
  }

  grid_link = NULL;


  if(!style || strcmp(style,"default")) {
    ctx->set_error(ctx,404, "received request with invalid style \"%s\" (expecting \"default\")",style);
    if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","style");
    return;
  }

  if(!matrixset) {
    ctx->set_error(ctx, 404, "received wmts request with no TILEMATRIXSET");
    if(kvp) ctx->set_exception(ctx,"MissingParameterValue","TileMatrixSet");
    return;
  } else {
    int i;
    for(i=0; i<tileset->grid_links->nelts; i++) {
      mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
      if(strcmp(sgrid->grid->name,matrixset)) continue;
      grid_link = sgrid;
      break;
    }
    if(!grid_link) {
      ctx->set_error(ctx, 404, "received wmts request with invalid TILEMATRIXSET %s",matrixset);
      if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","TileMatrixSet");
      return;
    }
  }

  if(!matrix) {
    ctx->set_error(ctx, 404, "received wmts request with no TILEMATRIX");
    if(kvp) ctx->set_exception(ctx,"MissingParameterValue","TileMatrix");
    return;
  } else {
    char *endptr;
    level = (int)strtol(matrix,&endptr,10);
    if(*endptr != 0 || level < grid_link->minz || level >= grid_link->maxz) {
      ctx->set_error(ctx, 404, "received wmts request with invalid TILEMATRIX %s", matrix);
      if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","TileMatrix");
      return;
    }
  }

  if(!tilerow) {
    ctx->set_error(ctx, 404, "received wmts request with no TILEROW");
    if(kvp) ctx->set_exception(ctx,"MissingParameterValue","TileRow");
    return;
  } else {
    char *endptr;
    row = (int)strtol(tilerow,&endptr,10);
    if(*endptr != 0 || row < 0) {
      ctx->set_error(ctx, 404, "received wmts request with invalid TILEROW %s",tilerow);
      if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","TileRow");
      return;
    }
  }

  if(!tilecol) {
    ctx->set_error(ctx, 404, "received wmts request with no TILECOL");
    if(kvp) ctx->set_exception(ctx,"MissingParameterValue","TileCol");
    return;
  } else {
    char *endptr;
    col = (int)strtol(tilecol,&endptr,10);
    if(endptr == tilecol || col < 0) {
      ctx->set_error(ctx, 404, "received wmts request with invalid TILECOL %s",tilecol);
      if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","TileCol");
      return;
    }
  }

  /* compute the x,y of the request depending on the grid origin */
  switch(grid_link->grid->origin) {
    case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
      x = col;
      y = grid_link->grid->levels[level]->maxy - row - 1;
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
      x = col;
      y = row;
      break;
    case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT:
      x = grid_link->grid->levels[level]->maxx - col - 1;
      y = grid_link->grid->levels[level]->maxy - row - 1;
      break;
    case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:
      x = grid_link->grid->levels[level]->maxx - col - 1;
      y = row;
      break;
    default:
      ctx->set_error(ctx,500,"BUG: invalid grid origin");
      return;
  }


  if(fi_j) {
    //we need the extent of the request, compute it here
    extent = apr_pcalloc(ctx->pool, sizeof(mapcache_extent));
    mapcache_grid_get_tile_extent(ctx,grid_link->grid,x,y,level,extent);
  }

  if(!fi_j) { /*we have a getTile request*/

#ifdef PEDANTIC_WMTS_FORMAT_CHECK
    if(tileset->format) {
      if(!format && !extension) {
        ctx->set_error(ctx, 404, "received wmts request with no format");
        return;
      } else {
        if(format && tileset->format && strcmp(format,tileset->format->mime_type)) {
          ctx->set_error(ctx, 404, "received wmts request with invalid format \"%s\" (expecting %s)",
                         format,tileset->format->mime_type);
          return;
        }
        if(extension && tileset->format && strcmp(extension,tileset->format->extension)) {
          ctx->set_error(ctx, 404, "received wmts request with invalid extension \"%s\" (expecting %s)",
                         extension,tileset->format->extension);
          return;
        }
      }
    }
#endif


    mapcache_request_get_tile *req = (mapcache_request_get_tile*)apr_pcalloc(
                                       ctx->pool,sizeof(mapcache_request_get_tile));
    
    ((mapcache_request*)req)->type = MAPCACHE_REQUEST_GET_TILE;
    req->ntiles = 1;
    req->tiles = (mapcache_tile**)apr_pcalloc(ctx->pool,req->ntiles * sizeof(mapcache_tile*));

    req->tiles[0] = mapcache_tileset_tile_create(ctx->pool, tileset, grid_link);
    if(!req->tiles[0]) {
      ctx->set_error(ctx, 500, "failed to allocate tile");
      if(kvp) ctx->set_exception(ctx,"NoApplicableCode","");
      return;
    }

    /*populate dimensions*/
    if(tileset->dimensions) {
      int d;
      for(d=0; d<tileset->dimensions->nelts; d++) {
        mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,d,mapcache_dimension*);
        const char *value = apr_table_get(dimtable,dimension->name);
        if(value) {
          mapcache_tile_set_requested_dimension(ctx,req->tiles[0],dimension->name,value);
        }
      }
    }
    req->tiles[0]->z = level;
    req->tiles[0]->x = x;
    req->tiles[0]->y = y;
    /* no need to validate all the tiles as they all have the same x,y,z */
    mapcache_tileset_tile_validate_z(ctx,req->tiles[0]);
    if(GC_HAS_ERROR(ctx)) {
      if(kvp) ctx->set_exception(ctx,"InvalidParameterValue","TileMatrix");
      return;
    }
    mapcache_tileset_tile_validate_x(ctx,req->tiles[0]);
    if(GC_HAS_ERROR(ctx)) {
      if(kvp) ctx->set_exception(ctx,"TileOutOfRange","TileCol");
      return;
    }
    mapcache_tileset_tile_validate_y(ctx,req->tiles[0]);
    if(GC_HAS_ERROR(ctx)) {
      if(kvp) ctx->set_exception(ctx,"TileOutOfRange","TileRow");
      return;
    }

    *request = (mapcache_request*)req;
    return;
  } else { /* we have a featureinfo request */
    mapcache_request_get_feature_info *req_fi;
    mapcache_feature_info *fi;
    if(!fi_i || (!infoformat && !extension)) {
      ctx->set_error(ctx,400,"received wmts featureinfo request with missing i,j, or format");
      return;
    }


    if(!tileset->source || !tileset->source->info_formats) {
      ctx->set_error(ctx,400,"tileset %s does not support featureinfo requests", tileset->name);
      if(kvp) ctx->set_exception(ctx,"OperationNotSupported","");
      return;
    }
    req_fi = (mapcache_request_get_feature_info*)apr_pcalloc(
               ctx->pool, sizeof(mapcache_request_get_feature_info));
    req_fi->request.type = MAPCACHE_REQUEST_GET_FEATUREINFO;
    fi = mapcache_tileset_feature_info_create(ctx->pool, tileset, grid_link);
    req_fi->fi = fi;
    if(infoformat) {
      fi->format = (char*)infoformat;
    }
    if(extension) {
      int fi_index = (int)strtol(extension,&endptr,10);
      if(endptr == extension || fi_index < 0 || fi_index >= tileset->source->info_formats->nelts) {
        ctx->set_error(ctx, 404, "received wmts featureinfo request with invalid extension %s",extension);
        return;
      }
      fi->format = APR_ARRAY_IDX(tileset->source->info_formats,fi_index,char*);
    }


    fi->i = (int)strtol(fi_i,&endptr,10);
    if(*endptr != 0 || fi->i < 0 || fi->i >= grid_link->grid->tile_sx) {
      ctx->set_error(ctx, 404, "received wmts featureinfo request with invalid I %s",fi_i);
      if(kvp) ctx->set_exception(ctx,"PointIJOutOfRange","i");
      return;
    }
    fi->j = (int)strtol(fi_j,&endptr,10);
    if(*endptr != 0 || fi->j < 0 || fi->j >= grid_link->grid->tile_sy) {
      ctx->set_error(ctx, 404, "received wmts featureinfo request with invalid J %s",fi_j);
      if(kvp) ctx->set_exception(ctx,"PointIJOutOfRange","j");
      return;
    }
    fi->map.width = grid_link->grid->tile_sx;
    fi->map.height = grid_link->grid->tile_sy;

    if(tileset->dimensions) {
      int d;
      for(d=0; d<tileset->dimensions->nelts; d++) {
        mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,d,mapcache_dimension*);
        const char *value = apr_table_get(dimtable,dimension->name);
        if(value) {
          mapcache_map_set_requested_dimension(ctx,&fi->map,dimension->name,value);
        }
      }
    }

    assert(extent);
    fi->map.extent = *extent;
    *request = (mapcache_request*)req_fi;
  }
}

void _error_report_wmts(mapcache_context *ctx, mapcache_service *service, char *msg,
                        char **err_body, apr_table_t *headers)
{
  char *exceptions="";
  const apr_array_header_t *array;
  apr_table_entry_t *elts;
  int i;
  char *template = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
   <ExceptionReport xmlns=\"http://www.opengis.net/ows/2.0\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.opengis.net/ows/2.0 owsExceptionReport.xsd\" version=\"1.0.0\" xml:lang=\"en\">\
   <!-- %s -->\
   %s\
</ExceptionReport>";
  if(!ctx->exceptions) {
    *err_body = msg;
    return;
  }
  exceptions="";

  array = apr_table_elts(ctx->exceptions);
  elts = (apr_table_entry_t *) array->elts;

  for (i = 0; i < array->nelts; i++) {
    exceptions = apr_pstrcat(ctx->pool,exceptions,apr_psprintf(ctx->pool,
                             "<Exception exceptionCode=\"%s\" locator=\"%s\"/>",elts[i].key,elts[i].val),NULL);
  }

  *err_body = apr_psprintf(ctx->pool,template,
                           mapcache_util_str_xml_escape(ctx->pool, msg, MAPCACHE_UTIL_XML_SECTION_COMMENT),
                           exceptions);
  apr_table_set(headers, "Content-Type", "application/xml");


}

mapcache_service* mapcache_service_wmts_create(mapcache_context *ctx)
{
  mapcache_service_wmts* service = (mapcache_service_wmts*)apr_pcalloc(ctx->pool, sizeof(mapcache_service_wmts));
  if(!service) {
    ctx->set_error(ctx, 500, "failed to allocate wtms service");
    return NULL;
  }
  service->service.url_prefix = apr_pstrdup(ctx->pool,"wmts");
  service->service.name = apr_pstrdup(ctx->pool,"wmts");
  service->service.type = MAPCACHE_SERVICE_WMTS;
  service->service.parse_request = _mapcache_service_wmts_parse_request;
  service->service.create_capabilities_response = _create_capabilities_wmts;
  service->service.format_error = _error_report_wmts;
  return (mapcache_service*)service;
}

/** @} */
/* vim: ts=2 sts=2 et sw=2
*/
