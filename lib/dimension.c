/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: OGC dimensions
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
#include <sys/types.h>
#ifdef USE_PCRE
#include <pcre.h>
#else
#include <regex.h>
#endif

typedef struct mapcache_dimension_values mapcache_dimension_values;
typedef struct mapcache_dimension_regex mapcache_dimension_regex;
typedef struct mapcache_dimension_composite mapcache_dimension_composite;

struct mapcache_dimension_values {
  mapcache_dimension dimension;
  apr_array_header_t *values;
  int case_sensitive;
};


struct mapcache_dimension_regex {
  mapcache_dimension dimension;
  char *regex_string;
#ifdef USE_PCRE
  pcre *pcregex;
#else
  regex_t *regex;
#endif
};



apr_array_header_t *mapcache_requested_dimensions_clone(apr_pool_t *pool, apr_array_header_t *src) {
  apr_array_header_t *ret = NULL;
  if(src) {
    int i;
    ret = apr_array_make(pool,src->nelts,sizeof(mapcache_requested_dimension*));
    for(i=0; i<src->nelts; i++) {
      mapcache_requested_dimension *tiledim = apr_pcalloc(pool,sizeof(mapcache_requested_dimension));
      mapcache_requested_dimension *srcdim = APR_ARRAY_IDX(src,i,mapcache_requested_dimension*);
      *tiledim = *srcdim;
      APR_ARRAY_PUSH(ret,mapcache_requested_dimension*) = tiledim;
    }
  }
  return ret;
}

void mapcache_set_requested_dimension(mapcache_context *ctx, apr_array_header_t *dimensions, const char *name, const char *value) {
  int i;
  if(!dimensions || dimensions->nelts <= 0) {
    ctx->set_error(ctx,500,"BUG: no dimensions configure for tile/map");
    return;
  }
  for(i=0;i<dimensions->nelts;i++) {
    mapcache_requested_dimension *dim = APR_ARRAY_IDX(dimensions,i,mapcache_requested_dimension*);
    if(!strcasecmp(dim->dimension->name,name)) {
      dim->requested_value = value?apr_pstrdup(ctx->pool,value):NULL;
      return;
    }
  }
  ctx->set_error(ctx,500,"BUG: dimension (%s) not found in tile/map",name);
}

void mapcache_set_cached_dimension(mapcache_context *ctx, apr_array_header_t *dimensions, const char *name, const char *value) {
  int i;
  if(!dimensions || dimensions->nelts <= 0) {
    ctx->set_error(ctx,500,"BUG: no dimensions configure for tile/map");
    return;
  }
  for(i=0;i<dimensions->nelts;i++) {
    mapcache_requested_dimension *dim = APR_ARRAY_IDX(dimensions,i,mapcache_requested_dimension*);
    if(!strcasecmp(dim->dimension->name,name)) {
      dim->cached_value = value?apr_pstrdup(ctx->pool,value):NULL;
      return;
    }
  }
  ctx->set_error(ctx,500,"BUG: dimension (%s) not found in tile/map",name);
}

void mapcache_tile_set_cached_dimension(mapcache_context *ctx, mapcache_tile *tile, const char *name, const char *value) {
  mapcache_set_cached_dimension(ctx,tile->dimensions,name,value);
}

void mapcache_map_set_cached_dimension(mapcache_context *ctx, mapcache_map *map, const char *name, const char *value) {
  mapcache_set_cached_dimension(ctx,map->dimensions,name,value);
}
void mapcache_tile_set_requested_dimension(mapcache_context *ctx, mapcache_tile *tile, const char *name, const char *value) {
  mapcache_set_requested_dimension(ctx,tile->dimensions,name,value);
}

void mapcache_map_set_requested_dimension(mapcache_context *ctx, mapcache_map *map, const char *name, const char *value) {
  mapcache_set_requested_dimension(ctx,map->dimensions,name,value);
}

static apr_array_header_t* _mapcache_dimension_regex_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dim, const char *value,
                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_regex *dimension = (mapcache_dimension_regex*)dim;
  apr_array_header_t *values = apr_array_make(ctx->pool,1,sizeof(char*));
#ifdef USE_PCRE
  int ovector[30];
  int rc = pcre_exec(dimension->pcregex,NULL,value,strlen(value),0,0,ovector,30);
  if(rc>0) {
    APR_ARRAY_PUSH(values,char*) = apr_pstrdup(ctx->pool,value);
  }
#else
  if(!regexec(dimension->regex,value,0,0,0)) {
    APR_ARRAY_PUSH(values,char*) = apr_pstrdup(ctx->pool,value);
  }
#endif
  else {
    ctx->set_error(ctx,400,"failed to validate requested value for dimension (%s)",dim->name);
  }
  return values;
}

static apr_array_header_t* _mapcache_dimension_regex_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_regex *dimension = (mapcache_dimension_regex*)dim;
  apr_array_header_t *ret = apr_array_make(ctx->pool,1,sizeof(char*));
  APR_ARRAY_PUSH(ret,char*) = apr_pstrdup(ctx->pool,dimension->regex_string);
  return ret;
}


static void _mapcache_dimension_regex_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  mapcache_dimension_regex *dimension;
  ezxml_t child_node = ezxml_child(node,"regex");
  
  dimension = (mapcache_dimension_regex*)dim;
  
  if(child_node && child_node->txt && *child_node->txt) {
    dimension->regex_string = apr_pstrdup(ctx->pool,child_node->txt);
  } else {
    ctx->set_error(ctx,400,"failed to parse dimension regex: no <regex> child supplied");
    return;
  }
#ifdef USE_PCRE
  {
    const char *pcre_err;
    int pcre_offset;
    dimension->pcregex = pcre_compile(dimension->regex_string,0,&pcre_err, &pcre_offset,0);
    if(!dimension->pcregex) {
      ctx->set_error(ctx,400,"failed to compile regular expression \"%s\" for dimension \"%s\": %s",
                     dimension->regex_string,dim->name,pcre_err);
      return;
    }
  }
#else
  {
    int rc = regcomp(dimension->regex, dimension->regex_string, REG_EXTENDED);
    if(rc) {
      char errmsg[200];
      regerror(rc,dimension->regex,errmsg,200);
      ctx->set_error(ctx,400,"failed to compile regular expression \"%s\" for dimension \"%s\": %s",
                     dimension->regex_string,dim->name,errmsg);
      return;
    }
  }
#endif

}

static apr_array_header_t* _mapcache_dimension_values_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dim, const char *value,
                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  int i;
  mapcache_dimension_values *dimension = (mapcache_dimension_values*)dim;
  apr_array_header_t *values = apr_array_make(ctx->pool,1,sizeof(char*));
  for(i=0; i<dimension->values->nelts; i++) {
    char *cur_val = APR_ARRAY_IDX(dimension->values,i,char*);
    if(dimension->case_sensitive) {
      if(!strcmp(value,cur_val)) {
        APR_ARRAY_PUSH(values,char*) = apr_pstrdup(ctx->pool,value);
        break;
      }
    } else {
      if(!strcasecmp(value,cur_val)) {
        APR_ARRAY_PUSH(values,char*) = apr_pstrdup(ctx->pool,value);
        break;
      }
    }
  }
  if(i == dimension->values->nelts) {
    ctx->set_error(ctx,400,"failed to validate requested value for dimension (%s)",dim->name);
  }
  return values;
}

static apr_array_header_t* _mapcache_dimension_values_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_values *dimension = (mapcache_dimension_values*)dim;
  apr_array_header_t *ret = apr_array_make(ctx->pool,dimension->values->nelts,sizeof(char*));
  int i;
  for(i=0; i<dimension->values->nelts; i++) {
    APR_ARRAY_PUSH(ret,char*) = apr_pstrdup(ctx->pool,APR_ARRAY_IDX(dimension->values,i,char*));
  }
  return ret;
}


static void _mapcache_dimension_values_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  mapcache_dimension_values *dimension;
  ezxml_t child_node = ezxml_child(node,"value");
  dimension = (mapcache_dimension_values*)dim;
  
  if(!child_node) {
    ctx->set_error(ctx,400,"failed to parse dimension values: no <value> children supplied");
    return;
  }
  for(; child_node; child_node = child_node->next) {
    const char* entry = child_node->txt;
    if(!entry || !*entry) {
      ctx->set_error(ctx,400,"failed to parse dimension values: empty <value>");
      return;
    }
    APR_ARRAY_PUSH(dimension->values,char*) = apr_pstrdup(ctx->pool,entry);
  }

  child_node = ezxml_child(node,"case_sensitive");
  if(child_node && child_node->txt) {
    if(!strcasecmp(child_node->txt,"true")) {
      dimension->case_sensitive = 1;
    }
  }

  if(!dimension->values->nelts) {
    ctx->set_error(ctx, 400, "<dimension> \"%s\" has no values",dim->name);
    return;
  }
}


apr_array_header_t* mapcache_dimension_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dimension, const char *value,
                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  if(!dimension->isTime) {
    return dimension->_get_entries_for_value(ctx, dimension, value, tileset, extent, grid);
  } else {
    return mapcache_dimension_time_get_entries_for_value(ctx, dimension, value, tileset, extent, grid);
  }
}

mapcache_dimension* mapcache_dimension_values_create(mapcache_context *ctx, apr_pool_t *pool)
{
  mapcache_dimension_values *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_values));
  dimension->dimension.type = MAPCACHE_DIMENSION_VALUES;
  dimension->values = apr_array_make(pool,1,sizeof(char*));
  dimension->dimension._get_entries_for_value = _mapcache_dimension_values_get_entries_for_value;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_values_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_values_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_values_get_all_entries;
  return (mapcache_dimension*)dimension;
}

mapcache_dimension* mapcache_dimension_regex_create(mapcache_context *ctx, apr_pool_t *pool)
{
  mapcache_dimension_regex *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_regex));
  dimension->dimension.type = MAPCACHE_DIMENSION_REGEX;
#ifndef USE_PCRE
  dimension->regex = (regex_t*)apr_pcalloc(pool, sizeof(regex_t));
#endif
  dimension->dimension._get_entries_for_value = _mapcache_dimension_regex_get_entries_for_value;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_regex_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_regex_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_regex_get_all_entries;
  return (mapcache_dimension*)dimension;
}



/* vim: ts=2 sts=2 et sw=2
*/
