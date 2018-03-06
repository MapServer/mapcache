/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: ElasticSearch dimension support
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2018 Regents of the University of Minnesota.
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
#include <float.h>

typedef struct mapcache_dimension_elasticsearch mapcache_dimension_elasticsearch;

struct mapcache_dimension_elasticsearch {
  mapcache_dimension dimension;
  mapcache_http *http;
  char *get_values_for_entry_query;
  char *get_all_values_query;
};


static void _mapcache_dimension_elasticsearch_parse_xml(mapcache_context *ctx, mapcache_dimension *dim, ezxml_t node)
{
  mapcache_dimension_elasticsearch *dimension;
  ezxml_t child;

  dimension = (mapcache_dimension_elasticsearch *)dim;

  child = ezxml_child(node,"http");
  if (child) {
    dimension->http = mapcache_http_configuration_parse_xml(ctx,child);
  } else {
    ctx->set_error(ctx,400,"elasticsearch dimension \"%s\" has no <http> node",dim->name);
    return;
  }
  child = ezxml_child(node,"validate_query");
  if(child) {
    dimension->get_values_for_entry_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"elasticsearch dimension \"%s\" has no <validate_query> node", dim->name);
    return;
  }
  child = ezxml_child(node,"list_query");
  if(child) {
    dimension->get_all_values_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"elasticsearch dimension \"%s\" has no <list_query> node", dim->name);
    return;
  }
}


static char * _replace_double(apr_pool_t*pool,const char*a,const char*b,double c)
{
  return mapcache_util_str_replace(pool,a,b,apr_psprintf(pool,"%.*e",DBL_DIG,c));
}


static char * _mapcache_dimension_elasticsearch_bind_parameters(mapcache_context *ctx, const char * req, const char * value,
  mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  char * res;

  res = mapcache_util_str_replace(ctx->pool,req,"$dim",value);

  if (tileset) res = mapcache_util_str_replace(ctx->pool,res,"$tileset",tileset->name);

  if (grid) res = mapcache_util_str_replace(ctx->pool,res,"$gridsrs",grid->srs);

  res = _replace_double(ctx->pool,res,"$minx",extent?extent->minx:-DBL_MAX);
  res = _replace_double(ctx->pool,res,"$miny",extent?extent->miny:-DBL_MAX);
  res = _replace_double(ctx->pool,res,"$maxx",extent?extent->maxx:DBL_MAX);
  res = _replace_double(ctx->pool,res,"$maxy",extent?extent->maxy:DBL_MAX);

  return res;
}


static apr_array_header_t * _mapcache_dimension_elasticsearch_do_query(mapcache_context * ctx, mapcache_http * http, const char * query)
{
  int i,nelts;
  char * resp;
  cJSON * json_resp;
  cJSON * json_sub;
  cJSON * json_item;

  apr_array_header_t *table = apr_array_make(ctx->pool,0,sizeof(char*));
  mapcache_buffer * buffer = mapcache_buffer_create(1,ctx->pool);
  mapcache_http * req = mapcache_http_clone(ctx,http);
  req->post_body = apr_pstrdup(ctx->pool,query);
  req->post_len = strlen(req->post_body);

  mapcache_http_do_request(ctx,req,buffer,NULL,NULL);
  if (GC_HAS_ERROR(ctx)) {
    return table;
  }

  mapcache_buffer_append(buffer,1,"");
  resp = (char*)buffer->buf;

  json_resp = cJSON_Parse(resp);
  json_sub = cJSON_GetObjectItem(json_resp,"aggregations");
  json_sub = cJSON_GetArrayItem(json_sub,0);
  json_sub = cJSON_GetObjectItem(json_sub,"buckets");
  if (!json_sub) {
    ctx->set_error(ctx,500,"elasticsearch dimension backend failed on query response: %s",resp);
  } else {
    nelts = cJSON_GetArraySize(json_sub);
    for (i=0 ; i<nelts ; i++)
    {
      json_item = cJSON_GetArrayItem(json_sub,i);
      json_item = cJSON_GetObjectItem(json_item,"key");
      APR_ARRAY_PUSH(table,char*) = apr_pstrdup(ctx->pool, cJSON_GetStringValue(json_item));
    }
  }
  cJSON_Delete(json_resp);
  return table;
}


static apr_array_header_t * _mapcache_dimension_elasticsearch_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
  mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_elasticsearch *dimension = (mapcache_dimension_elasticsearch*)dim;
  char * req = dimension->get_all_values_query;
  char * res = _mapcache_dimension_elasticsearch_bind_parameters(ctx,req,NULL,tileset,extent,grid);
  apr_array_header_t *table = _mapcache_dimension_elasticsearch_do_query(ctx,dimension->http,res);

  return table;
}


static apr_array_header_t* _mapcache_dimension_elasticsearch_get_entries_for_value(mapcache_context *ctx,
  mapcache_dimension *dim, const char *value, mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_elasticsearch *dimension = (mapcache_dimension_elasticsearch*)dim;
  char * req = dimension->get_values_for_entry_query;
  char * res = _mapcache_dimension_elasticsearch_bind_parameters(ctx,req,value,tileset,extent,grid);
  apr_array_header_t *table = _mapcache_dimension_elasticsearch_do_query(ctx,dimension->http,res);

  return table;
}


mapcache_dimension* mapcache_dimension_elasticsearch_create(mapcache_context *ctx, apr_pool_t *pool)
{
  mapcache_dimension_elasticsearch *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_elasticsearch));
  dimension->dimension.type = MAPCACHE_DIMENSION_ELASTICSEARCH;
  dimension->http = NULL;
  dimension->dimension._get_entries_for_value = _mapcache_dimension_elasticsearch_get_entries_for_value;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_elasticsearch_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_elasticsearch_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_elasticsearch_get_all_entries;
  return (mapcache_dimension*)dimension;
}


/* vim: ts=2 sts=2 et sw=2
*/
