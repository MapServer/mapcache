/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: ElasticSearch dimension support
 * Author:   Jerome Boue and the MapServer team.
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
#include "cJSON.h"

typedef struct mapcache_dimension_elasticsearch mapcache_dimension_elasticsearch;

struct mapcache_dimension_elasticsearch {
  mapcache_dimension dimension;
  mapcache_http *http;
  char *get_values_for_entry_query;
  char *get_all_values_query;
  char *response_format_to_validate_query;
  char *response_format_to_list_query;
};


// Hook cJSON memory allocation on APR pool mechanism
static apr_pool_t * _pool_for_cJSON_malloc_hook;
static void * _malloc_for_cJSON(size_t size) {
  return apr_palloc(_pool_for_cJSON_malloc_hook,size);
}
static void _free_for_cJSON(void *ptr) { }
static void _create_json_pool(apr_pool_t * parent_pool) {
  cJSON_Hooks hooks = { _malloc_for_cJSON, _free_for_cJSON };
  apr_pool_create(&_pool_for_cJSON_malloc_hook,parent_pool);
  cJSON_InitHooks(&hooks);
}
static void _destroy_json_pool() {
  apr_pool_destroy(_pool_for_cJSON_malloc_hook);
}


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
  child = ezxml_child(node,"validate_response");
  if(child) {
    dimension->response_format_to_validate_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"elasticsearch dimension \"%s\" has no <validate_response> node", dim->name);
    return;
  }
  child = ezxml_child(node,"list_response");
  if(child) {
    dimension->response_format_to_list_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"elasticsearch dimension \"%s\" has no <list_response> node", dim->name);
    return;
  }
}


static char * _mapcache_dimension_elasticsearch_bind_parameters(mapcache_context *ctx, const char * req, const char * value,
  time_t start, time_t end, mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  char * res;
  char * val = NULL;

  if (value) {
    // Sanitize dimension value for safe insertion in JSON request
    cJSON * json_str;
    _create_json_pool(ctx->pool);
    json_str = cJSON_CreateString(value);
    val = cJSON_Print(json_str);
    if (val) {
      // Discard double quotes while copying
      val = apr_pstrndup(ctx->pool,val+1,strlen(val)-2);
    }
    _destroy_json_pool();
  }
  res = mapcache_util_str_replace_all(ctx->pool,req,":dim",val);

  if (tileset) res = mapcache_util_str_replace_all(ctx->pool,res,":tileset",tileset->name);

  if (grid) res = mapcache_util_str_replace_all(ctx->pool,res,":gridsrs",grid->srs);

  res = mapcache_util_dbl_replace_all(ctx->pool,res,":minx",extent?extent->minx:-DBL_MAX);
  res = mapcache_util_dbl_replace_all(ctx->pool,res,":miny",extent?extent->miny:-DBL_MAX);
  res = mapcache_util_dbl_replace_all(ctx->pool,res,":maxx",extent?extent->maxx:DBL_MAX);
  res = mapcache_util_dbl_replace_all(ctx->pool,res,":maxy",extent?extent->maxy:DBL_MAX);

  res = mapcache_util_str_replace_all(ctx->pool,res,":start_timestamp",apr_psprintf(ctx->pool,"%ld",start*1000));
  res = mapcache_util_str_replace_all(ctx->pool,res,":end_timestamp",apr_psprintf(ctx->pool,"%ld",end*1000));

  return res;
}


static apr_array_header_t * _mapcache_dimension_elasticsearch_do_query(mapcache_context * ctx, mapcache_http * http, const char * query, const char * response_format)
{
  char * resp;
  cJSON * json_resp;
  cJSON * json_fmt;
  cJSON * index;
  cJSON * extract;
  cJSON * item;
  cJSON * sub;

  apr_array_header_t *table = apr_array_make(ctx->pool,0,sizeof(char*));

  // Build and execute HTTP request
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

  _create_json_pool(ctx->pool);

  // Parse response format: this should be a list of keys or integers
  json_fmt = cJSON_Parse(response_format);
  if (!json_fmt) {
    ctx->set_error(ctx,500,"elasticsearch dimension backend failed on response format: %s",response_format);
    goto cleanup;
  }

  // Parse response
  json_resp = cJSON_Parse(resp);
  if (!json_resp) {
    ctx->set_error(ctx,500,"elasticsearch dimension backend failed on query response: %s",resp);
    goto cleanup;
  }

  // Analyze response according to response format
  extract = json_resp;
  cJSON_ArrayForEach(index,json_fmt) {
    char * key = index->valuestring;
    int    pos = index->valueint;

    // Key on Dict => return Dict[Key]
    if (cJSON_IsString(index) && cJSON_IsObject(extract)) {
      extract = cJSON_GetObjectItem(extract,key);

    // Index on List of lists => return [ list[Index] for list in List ]
    } else if (cJSON_IsNumber(index) && cJSON_IsArray(extract)
               && cJSON_IsArray(cJSON_GetArrayItem(extract,0))) {
      sub = cJSON_CreateArray();
      cJSON_ArrayForEach(item,extract) {
        cJSON * value = cJSON_GetArrayItem(item,pos);
        if (value) cJSON_AddItemToArray(sub,cJSON_Duplicate(value,1));
      }
      extract = sub;

    // Index on List => return List[Index]
    } else if (cJSON_IsNumber(index) && cJSON_IsArray(extract)) {
      extract = cJSON_GetArrayItem(extract,pos);

    // Key on List => return [ Dict[Key] for Dict in List ]
    } else if (cJSON_IsString(index) && cJSON_IsArray(extract)) {
      sub = cJSON_CreateArray();
      cJSON_ArrayForEach(item,extract) {
        cJSON * value = cJSON_GetObjectItem(item,key);
        if (value) cJSON_AddItemToArray(sub,cJSON_Duplicate(value,1));
      }
      extract = sub;

    } else {
        ctx->set_error(ctx,500,"elasticsearch dimension backend failed on query response: %s",resp);
        goto cleanup;
    }

  }

  if (!cJSON_IsArray(extract)) {
    item = extract;
    extract = cJSON_CreateArray();
    cJSON_AddItemToArray(extract,item);
  }
  cJSON_ArrayForEach(item,extract) {
    APR_ARRAY_PUSH(table,char*) = apr_pstrdup(ctx->pool, cJSON_GetStringValue(item));
  }

cleanup:
  _destroy_json_pool();
  return table;
}


static apr_array_header_t * _mapcache_dimension_elasticsearch_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
  mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_elasticsearch *dimension = (mapcache_dimension_elasticsearch*)dim;
  char * req = dimension->get_all_values_query;
  char * resp_fmt = dimension->response_format_to_list_query;
  char * res = _mapcache_dimension_elasticsearch_bind_parameters(ctx,req,NULL,0,0,tileset,extent,grid);
  apr_array_header_t *table = _mapcache_dimension_elasticsearch_do_query(ctx,dimension->http,res,resp_fmt);

  return table;
}


static apr_array_header_t* _mapcache_dimension_elasticsearch_get_entries_for_time_range(mapcache_context *ctx,
  mapcache_dimension *dim, const char *dim_value, time_t start, time_t end,
  mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_elasticsearch *dimension = (mapcache_dimension_elasticsearch*)dim;
  char * req = dimension->get_values_for_entry_query;
  char * resp_fmt = dimension->response_format_to_validate_query;
  char * res = _mapcache_dimension_elasticsearch_bind_parameters(ctx,req,dim_value,start,end,tileset,extent,grid);
  apr_array_header_t *table = _mapcache_dimension_elasticsearch_do_query(ctx,dimension->http,res,resp_fmt);

  return table;
}


static apr_array_header_t* _mapcache_dimension_elasticsearch_get_entries_for_value(mapcache_context *ctx,
  mapcache_dimension *dim, const char *value, mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  return _mapcache_dimension_elasticsearch_get_entries_for_time_range(ctx,dim,value,0,0,tileset,extent,grid);
}


mapcache_dimension* mapcache_dimension_elasticsearch_create(mapcache_context *ctx, apr_pool_t *pool)
{
  mapcache_dimension_elasticsearch *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_elasticsearch));
  dimension->dimension.type = MAPCACHE_DIMENSION_ELASTICSEARCH;
  dimension->http = NULL;
  dimension->dimension._get_entries_for_value = _mapcache_dimension_elasticsearch_get_entries_for_value;
  dimension->dimension._get_entries_for_time_range = _mapcache_dimension_elasticsearch_get_entries_for_time_range;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_elasticsearch_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_elasticsearch_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_elasticsearch_get_all_entries;
  return (mapcache_dimension*)dimension;
}


/* vim: ts=2 sts=2 et sw=2
*/
