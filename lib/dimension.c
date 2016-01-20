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
#include <apr_time.h>
#include <time.h>
#ifdef USE_SQLITE
#include <sqlite3.h>
#include <float.h>
#endif
#ifdef USE_PCRE
#include <pcre.h>
#else
#include <regex.h>
#endif

typedef struct mapcache_dimension_time mapcache_dimension_time;
typedef struct mapcache_dimension_time_sqlite mapcache_dimension_time_sqlite;
typedef struct mapcache_dimension_intervals mapcache_dimension_intervals;
typedef struct mapcache_dimension_values mapcache_dimension_values;
typedef struct mapcache_dimension_sqlite mapcache_dimension_sqlite;
typedef struct mapcache_dimension_regex mapcache_dimension_regex;
typedef struct mapcache_dimension_composite mapcache_dimension_composite;

struct mapcache_dimension_values {
  mapcache_dimension dimension;
  apr_array_header_t *values;
  int case_sensitive;
};

struct mapcache_dimension_sqlite {
  mapcache_dimension dimension;
  char *dbfile;
  char *get_values_for_entry_query;
  char *get_all_values_query;
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

struct mapcache_dimension_time {
  mapcache_dimension_sqlite dimension;
};


#ifndef HAVE_TIMEGM
time_t timegm(struct tm *tm)
{
  time_t t, tdiff;
  struct tm in, gtime, ltime;

  memcpy(&in, tm, sizeof(in));
  t = mktime(&in);

  memcpy(&gtime, gmtime(&t), sizeof(gtime));
  memcpy(&ltime, localtime(&t), sizeof(ltime));
  gtime.tm_isdst = ltime.tm_isdst;
  tdiff = t - mktime(&gtime);

  memcpy(&in, tm, sizeof(in));
  return mktime(&in) + tdiff;
}
#endif

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

mapcache_dimension* mapcache_dimension_values_create(mapcache_context *ctx, apr_pool_t *pool)
{
  mapcache_dimension_values *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_values));
  dimension->dimension.type = MAPCACHE_DIMENSION_VALUES;
  dimension->values = apr_array_make(pool,1,sizeof(char*));
  dimension->dimension.get_entries_for_value = _mapcache_dimension_values_get_entries_for_value;
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
  dimension->dimension.get_entries_for_value = _mapcache_dimension_regex_get_entries_for_value;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_regex_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_regex_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_regex_get_all_entries;
  return (mapcache_dimension*)dimension;
}

#ifdef USE_SQLITE

struct sqlite_dimension_conn {
  sqlite3 *handle;
  sqlite3_stmt **prepared_statements;
  int n_statements;
};

void mapcache_sqlite_dimension_connection_constructor(mapcache_context *ctx, void **conn_, void *params)
{
  int ret;
  int flags;
  char *dbfile = (char*) params;
  struct sqlite_dimension_conn *conn = calloc(1, sizeof (struct sqlite_dimension_conn));
  *conn_ = conn;
  flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
  ret = sqlite3_open_v2(dbfile, &conn->handle, flags, NULL);

  if (ret != SQLITE_OK) {
    ctx->set_error(ctx,500,"failed to open sqlite dimension dbfile (%s): %s",dbfile,sqlite3_errmsg(conn->handle));
    sqlite3_close(conn->handle);
    *conn_=NULL;
    return;
  }
  sqlite3_busy_timeout(conn->handle, 300000);
}

void mapcache_sqlite_dimension_connection_destructor(void *conn_)
{
  struct sqlite_dimension_conn *conn = (struct sqlite_dimension_conn*) conn_;
  while(conn->n_statements) {
    conn->n_statements--;
    if(conn->prepared_statements[conn->n_statements]) {
      sqlite3_finalize(conn->prepared_statements[conn->n_statements]);
    }
  }
  free(conn->prepared_statements);
  sqlite3_close(conn->handle);
  free(conn);
}

static mapcache_pooled_connection* _sqlite_dimension_get_conn(mapcache_context *ctx, mapcache_tileset *tileset, mapcache_dimension_sqlite *dim) {
  mapcache_dimension *pdim = (mapcache_dimension*)dim;
  char *conn_key = apr_pstrcat(ctx->pool,"dim_",tileset?tileset->name:"","_",pdim->name,NULL);
  mapcache_pooled_connection *pc = mapcache_connection_pool_get_connection(ctx,conn_key,
        mapcache_sqlite_dimension_connection_constructor,
        mapcache_sqlite_dimension_connection_destructor, dim->dbfile);
  return pc;
}

static void _sqlite_dimension_release_conn(mapcache_context *ctx, mapcache_pooled_connection *pc)
{
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
  } else {
    mapcache_connection_pool_release_connection(ctx,pc);
  }
}

static void _mapcache_dimension_sqlite_bind_parameters(mapcache_context *ctx, sqlite3_stmt *stmt, sqlite3 *handle,
                                                       const char *value,
                                                       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  int paramidx,ret;
  paramidx = sqlite3_bind_parameter_index(stmt, ":dim");
  if (paramidx) {
    ret = sqlite3_bind_text(stmt, paramidx, value, -1, SQLITE_STATIC);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "sqlite dimension failed to bind :dim : %s", sqlite3_errmsg(handle));
      return;
    }
  }

  if(tileset) {
    paramidx = sqlite3_bind_parameter_index(stmt, ":tileset");
    if (paramidx) {
      ret = sqlite3_bind_text(stmt, paramidx, tileset->name, -1, SQLITE_STATIC);
      if(ret != SQLITE_OK) {
        ctx->set_error(ctx,400, "sqlite dimension failed to bind :tileset : %s", sqlite3_errmsg(handle));
        return;
      }
    }
  }

  if(grid) {
    paramidx = sqlite3_bind_parameter_index(stmt, ":gridsrs");
    if (paramidx) {
      ret = sqlite3_bind_text(stmt, paramidx, grid->srs, -1, SQLITE_STATIC);
      if(ret != SQLITE_OK) {
        ctx->set_error(ctx,400, "failed to bind :gridsrs %s", sqlite3_errmsg(handle));
        return;
      }
    }
  }

  paramidx = sqlite3_bind_parameter_index(stmt, ":minx");
  if (paramidx) {
    ret = sqlite3_bind_double(stmt, paramidx, extent?extent->minx:-DBL_MAX);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :minx %s", sqlite3_errmsg(handle));
      return;
    }
  }
  paramidx = sqlite3_bind_parameter_index(stmt, ":miny");
  if (paramidx) {
    ret = sqlite3_bind_double(stmt, paramidx, extent?extent->miny:-DBL_MAX);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :miny %s", sqlite3_errmsg(handle));
      return;
    }
  }
  paramidx = sqlite3_bind_parameter_index(stmt, ":maxx");
  if (paramidx) {
    ret = sqlite3_bind_double(stmt, paramidx, extent?extent->maxx:DBL_MAX);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :maxx %s", sqlite3_errmsg(handle));
      return;
    }
  }
  paramidx = sqlite3_bind_parameter_index(stmt, ":maxy");
  if (paramidx) {
    ret = sqlite3_bind_double(stmt, paramidx, extent?extent->maxy:DBL_MAX);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :maxy %s", sqlite3_errmsg(handle));
      return;
    }
  }
}
static apr_array_header_t* _mapcache_dimension_sqlite_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dim, const char *value,
                                                                           mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_sqlite *dimension = (mapcache_dimension_sqlite*)dim;
  struct sqlite_dimension_conn *conn = NULL;
  mapcache_pooled_connection *pc;
  apr_array_header_t *values = apr_array_make(ctx->pool,1,sizeof(char*));
  int sqliteret;
  pc = _sqlite_dimension_get_conn(ctx,tileset,dimension);
  if (GC_HAS_ERROR(ctx)) {
    return values;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(2,sizeof(sqlite3_stmt*));
    conn->n_statements = 2;
  }
  if(!conn->prepared_statements[0]) {
    if(SQLITE_OK != sqlite3_prepare_v2(conn->handle, dimension->get_values_for_entry_query, -1, &conn->prepared_statements[0], NULL)) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      goto cleanup;
    }
  }

  _mapcache_dimension_sqlite_bind_parameters(ctx,conn->prepared_statements[0],conn->handle,value,
      tileset,extent,grid);
  if (GC_HAS_ERROR(ctx)) {
    return values;
  }


  do {
    sqliteret = sqlite3_step(conn->prepared_statements[0]);
    if (sqliteret != SQLITE_DONE && sqliteret != SQLITE_ROW && sqliteret != SQLITE_BUSY && sqliteret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on query : %s (%d)", sqlite3_errmsg(conn->handle), sqliteret);
      goto cleanup;
    }
    if(sqliteret == SQLITE_ROW) {
      const char* dimrow = (const char*) sqlite3_column_text(conn->prepared_statements[0], 0);
      APR_ARRAY_PUSH(values,char*) = apr_pstrdup(ctx->pool,dimrow);
    }
  } while (sqliteret == SQLITE_ROW || sqliteret == SQLITE_BUSY || sqliteret == SQLITE_LOCKED);

cleanup:
  if(conn->prepared_statements[0]) {
    sqlite3_reset(conn->prepared_statements[0]);
  }
  _sqlite_dimension_release_conn(ctx,pc);

  return values;
}

static apr_array_header_t* _mapcache_dimension_sqlite_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
                                mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid)
{
  mapcache_dimension_sqlite *dimension = (mapcache_dimension_sqlite*)dim;
  struct sqlite_dimension_conn *conn = NULL;
  int sqliteret;
  apr_array_header_t *ret = apr_array_make(ctx->pool,0,sizeof(char*));
  mapcache_pooled_connection *pc;
  pc = _sqlite_dimension_get_conn(ctx,tileset,dimension);
  if (GC_HAS_ERROR(ctx)) {
    return ret;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(2,sizeof(sqlite3_stmt*));
    conn->n_statements = 2;
  }

  if(!conn->prepared_statements[1]) {
    sqliteret = sqlite3_prepare_v2(conn->handle, dimension->get_all_values_query, -1, &conn->prepared_statements[1], NULL);
    if(sqliteret != SQLITE_OK) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      goto cleanup;
    }
  }
  _mapcache_dimension_sqlite_bind_parameters(ctx,conn->prepared_statements[1],conn->handle,NULL,tileset,extent,grid);
  if (GC_HAS_ERROR(ctx)) {
    return ret;
  }
  do {
    sqliteret = sqlite3_step(conn->prepared_statements[1]);
    if (sqliteret != SQLITE_DONE && sqliteret != SQLITE_ROW && sqliteret != SQLITE_BUSY && sqliteret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on query : %s (%d)", sqlite3_errmsg(conn->handle), sqliteret);
      goto cleanup;
    }
    if(sqliteret == SQLITE_ROW) {
      const char* sqdim = (const char*) sqlite3_column_text(conn->prepared_statements[1], 0);
      APR_ARRAY_PUSH(ret,char*) = apr_pstrdup(ctx->pool,sqdim);
    }
  } while (sqliteret == SQLITE_ROW || sqliteret == SQLITE_BUSY || sqliteret == SQLITE_LOCKED);

cleanup:
  if(conn->prepared_statements[1]) {
    sqlite3_reset(conn->prepared_statements[1]);
  }
  _sqlite_dimension_release_conn(ctx,pc);

  return ret;
}


static void _mapcache_dimension_sqlite_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  mapcache_dimension_sqlite *dimension;
  ezxml_t child;

  dimension = (mapcache_dimension_sqlite*)dim;

  child = ezxml_child(node,"dbfile");
  if(child) {
    dimension->dbfile = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"sqlite dimension \"%s\" has no <dbfile> node", dim->name);
    return;
  }
  child = ezxml_child(node,"validate_query");
  if(child) {
    dimension->get_values_for_entry_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"sqlite dimension \"%s\" has no <validate_query> node", dim->name);
    return;
  }
  child = ezxml_child(node,"list_query");
  if(child) {
    dimension->get_all_values_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"sqlite dimension \"%s\" has no <list_query> node", dim->name);
    return;
  }

}


static void _bind_sqlite_dimension_time_params(mapcache_context *ctx, sqlite3_stmt *stmt,
        sqlite3 *handle, const char *dim_value, mapcache_tileset *tileset, mapcache_grid *grid, mapcache_extent *extent,
        time_t start, time_t end)
{
  int paramidx,ret;
  
  _mapcache_dimension_sqlite_bind_parameters(ctx,stmt,handle,dim_value,tileset,extent,grid);

  paramidx = sqlite3_bind_parameter_index(stmt, ":start_timestamp");
  if (paramidx) {
    ret = sqlite3_bind_int64(stmt, paramidx, start);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :start_timestamp: %s", sqlite3_errmsg(handle));
      return;
    }
  }

  paramidx = sqlite3_bind_parameter_index(stmt, ":end_timestamp");
  if (paramidx) {
    ret = sqlite3_bind_int64(stmt, paramidx, end);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :end_timestamp: %s", sqlite3_errmsg(handle));
      return;
    }
  }
}

apr_array_header_t *_mapcache_dimension_time_get_entries(mapcache_context *ctx, mapcache_dimension_time *dim, const char *dim_value,
        mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid, time_t *intervals, int n_intervals) {
  mapcache_dimension_sqlite *sdim = (mapcache_dimension_sqlite*)dim;
  int i,ret;
  apr_array_header_t *time_ids = NULL;
  mapcache_pooled_connection *pc;
  struct sqlite_dimension_conn *conn;
  pc = _sqlite_dimension_get_conn(ctx,tileset,sdim);
  if (GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(1,sizeof(sqlite3_stmt*));
    conn->n_statements = 1;
  }
  if(!conn->prepared_statements[0]) {
    ret = sqlite3_prepare_v2(conn->handle, sdim->get_values_for_entry_query, -1, &conn->prepared_statements[0], NULL);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx, 500, "time sqlite backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      _sqlite_dimension_release_conn(ctx, pc);
      return NULL;
    }
  }
  
  for(i=0;i<n_intervals;i++) {
    _bind_sqlite_dimension_time_params(ctx,conn->prepared_statements[0],conn->handle,dim_value,tileset,grid,extent,intervals[i*2],intervals[i*2+1]);
    if(GC_HAS_ERROR(ctx)) {
        _sqlite_dimension_release_conn(ctx, pc);
        return NULL;
      }

    time_ids = apr_array_make(ctx->pool,0,sizeof(char*));
    do {
        ret = sqlite3_step(conn->prepared_statements[0]);
        if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
            ctx->set_error(ctx, 500, "sqlite backend failed on dimension_time query : %s (%d)", sqlite3_errmsg(conn->handle), ret);
            _sqlite_dimension_release_conn(ctx, pc);
            return NULL;
          }
        if(ret == SQLITE_ROW) {
            const char* time_id = (const char*) sqlite3_column_text(conn->prepared_statements[0], 0);
            APR_ARRAY_PUSH(time_ids,char*) = apr_pstrdup(ctx->pool,time_id);
          }
      } while (ret == SQLITE_ROW || ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
    sqlite3_reset(conn->prepared_statements[0]);
  }
  _sqlite_dimension_release_conn(ctx, pc);
  return time_ids;
}


apr_array_header_t* _mapcache_dimension_time_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
        mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  mapcache_dimension_time *tdim = (mapcache_dimension_time*)dim;
  time_t all[2] = {0,INT_MAX};
  return _mapcache_dimension_time_get_entries(ctx,tdim,NULL,tileset,extent,grid,all,1);
}
#endif

typedef enum {
  MAPCACHE_TINTERVAL_SECOND,
  MAPCACHE_TINTERVAL_MINUTE,
  MAPCACHE_TINTERVAL_HOUR,
  MAPCACHE_TINTERVAL_DAY,
  MAPCACHE_TINTERVAL_MONTH,
  MAPCACHE_TINTERVAL_YEAR
} mapcache_time_interval_t;


#ifdef USE_SQLITE
void _mapcache_dimension_time_parse_xml(mapcache_context *ctx, mapcache_dimension *dim, ezxml_t node) {
  mapcache_dimension_sqlite *sdim = (mapcache_dimension_sqlite*)dim;
  mapcache_dimension *pdim = (mapcache_dimension*)dim;
  ezxml_t child;

  child = ezxml_child(node,"dbfile");
  if(child && child->txt && *child->txt) {
    sdim->dbfile = apr_pstrdup(ctx->pool,child->txt);
  } else {
    ctx->set_error(ctx,400,"no <dbfile> entry for <dimension_time> %s",pdim->name);
    return;
  }
  child = ezxml_child(node,"query");
  if(child && child->txt && *child->txt) {
    sdim->get_values_for_entry_query = apr_pstrdup(ctx->pool,child->txt);
  } else {
    ctx->set_error(ctx,400,"no <query> entry for <dimension_time> %s",pdim->name);
    return;
  }
}
#endif

char *mapcache_ogc_strptime(const char *value, struct tm *ts, mapcache_time_interval_t *ti) {
  char *valueptr;
  memset (ts, '\0', sizeof (*ts));
  valueptr = strptime(value,"%Y-%m-%dT%H:%M:%SZ",ts);
  *ti = MAPCACHE_TINTERVAL_SECOND;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%dT%H:%MZ",ts);
  *ti = MAPCACHE_TINTERVAL_MINUTE;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%dT%HZ",ts);
  *ti = MAPCACHE_TINTERVAL_HOUR;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%d",ts);
  *ti = MAPCACHE_TINTERVAL_DAY;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m",ts);
  *ti = MAPCACHE_TINTERVAL_MONTH;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y",ts);
  *ti = MAPCACHE_TINTERVAL_YEAR;
  if(valueptr) return valueptr;
  return NULL;
}

#ifdef USE_SQLITE
apr_array_header_t* _mapcache_dimension_time_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dimension, const char *value,
                                                                   mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  
  /* look if supplied value is a predefined key */
  /* split multiple values, loop */

  /* extract start and end values */
  struct tm tm_start,tm_end;
  time_t *intervals;
  mapcache_time_interval_t tis,tie;
  char *valueptr = apr_pstrdup(ctx->pool,value);
  char *last,*key;
  int count=0;
  mapcache_dimension_time *dimension_time = (mapcache_dimension_time*)dimension;
  
  /*count how many time entries were supplied*/
  for(; *value; value++) if(*value == ',') count++;
  
  intervals = apr_pcalloc(ctx->pool,2*count*sizeof(time_t));
  count = 0;
  
  
  /* Split the input on '&' */
  for (key = apr_strtok(valueptr, ",", &last); key != NULL;
       key = apr_strtok(NULL, ",", &last)) {
    valueptr = mapcache_ogc_strptime(key,&tm_start,&tis);
    if(!valueptr) {
      ctx->set_error(ctx,400,"failed to parse time %s",value);
      return NULL;
    }
  
    if(*valueptr == '/' || (*valueptr == '-' && *(valueptr+1) == '-')) {
      /* we have a second (end) time */
      if (*valueptr == '/') {
        valueptr++;
      }
      else {
        valueptr += 2;
      }
      valueptr = mapcache_ogc_strptime(valueptr,&tm_end,&tie);
      if(!valueptr) {
        ctx->set_error(ctx,400,"failed to parse end time in %s",value);
        return NULL;
      }
    } else if(*valueptr == 0) {
      tie = tis;
      tm_end = tm_start;
    } else {
      ctx->set_error(ctx,400,"failed (2) to parse time %s",value);
      return NULL;
    }
    intervals[count*2+1] = timegm(&tm_end);
    intervals[count*2] = timegm(&tm_start);
    if(difftime(intervals[count*2],intervals[count*2+1]) == 0) {
      switch(tie) {
      case MAPCACHE_TINTERVAL_SECOND:
        tm_end.tm_sec += 1;
        break;
      case MAPCACHE_TINTERVAL_MINUTE:
        tm_end.tm_min += 1;
        break;
      case MAPCACHE_TINTERVAL_HOUR:
        tm_end.tm_hour += 1;
        break;
      case MAPCACHE_TINTERVAL_DAY:
        tm_end.tm_mday += 1;
        break;
      case MAPCACHE_TINTERVAL_MONTH:
        tm_end.tm_mon += 1;
        break;
      case MAPCACHE_TINTERVAL_YEAR:
        tm_end.tm_year += 1;
        break;
      }
      intervals[count*2+1] = timegm(&tm_end);
    }
    count++;
  }
  return _mapcache_dimension_time_get_entries(ctx,dimension_time,value,tileset,extent,grid,intervals,count); 
  /* end loop */
}
#endif

mapcache_dimension* mapcache_dimension_sqlite_create(mapcache_context *ctx, apr_pool_t *pool)
{
#ifdef USE_SQLITE
  mapcache_dimension_sqlite *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_sqlite));
  dimension->dimension.type = MAPCACHE_DIMENSION_SQLITE;
  dimension->dbfile = NULL;
  dimension->dimension.get_entries_for_value = _mapcache_dimension_sqlite_get_entries_for_value;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_sqlite_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_sqlite_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_sqlite_get_all_entries;
  return (mapcache_dimension*)dimension;
#else
  ctx->set_error(ctx,400,"Sqlite dimension support requires SQLITE support to be built in");
  return NULL;
#endif
}


mapcache_dimension* mapcache_dimension_time_create(mapcache_context *ctx, apr_pool_t *pool) {
#ifdef USE_SQLITE
  mapcache_dimension_time *dim = apr_pcalloc(pool, sizeof(mapcache_dimension_time));
  mapcache_dimension *pdim = (mapcache_dimension*)dim;
  pdim->get_entries_for_value = _mapcache_dimension_time_get_entries_for_value;
  pdim->get_all_entries = _mapcache_dimension_time_get_all_entries;
  pdim->get_all_ogc_formatted_entries = _mapcache_dimension_time_get_all_entries;
  pdim->configuration_parse_xml = _mapcache_dimension_time_parse_xml;
  return pdim;
#else
  ctx->set_error(ctx,400,"TIME dimension support requires SQLITE support to be built in");
  return NULL;
#endif
}

/* vim: ts=2 sts=2 et sw=2
*/
