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



static int _mapcache_dimension_intervals_validate(mapcache_context *ctx, mapcache_dimension *dim, char **value)
{
  int i;
  char *endptr;
  mapcache_dimension_intervals *dimension;
  double val = strtod(*value,&endptr);
  *value = apr_psprintf(ctx->pool,"%g",val);
  if(*endptr != 0) {
    return MAPCACHE_FAILURE;
  }
  dimension = (mapcache_dimension_intervals*)dim;
  for(i=0; i<dimension->nintervals; i++) {
    double diff;
    mapcache_interval *interval = &dimension->intervals[i];
    if(val<interval->start || val>interval->end)
      continue;
    if(interval->resolution == 0)
      return MAPCACHE_SUCCESS;
    diff = fmod((val - interval->start),interval->resolution);
    if(diff == 0.0)
      return MAPCACHE_SUCCESS;
  }
  return MAPCACHE_FAILURE;
}

static apr_array_header_t* _mapcache_dimension_intervals_print(mapcache_context *ctx, mapcache_dimension *dim)
{
  mapcache_dimension_intervals *dimension = (mapcache_dimension_intervals*)dim;
  apr_array_header_t *ret = apr_array_make(ctx->pool,dimension->nintervals,sizeof(char*));
  int i;
  for(i=0; i<dimension->nintervals; i++) {
    mapcache_interval *interval = &dimension->intervals[i];
    APR_ARRAY_PUSH(ret,char*) = apr_psprintf(ctx->pool,"%g/%g/%g",interval->start,interval->end,interval->resolution);
  }
  return ret;
}


static void _mapcache_dimension_intervals_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  mapcache_dimension_intervals *dimension;
  char *key,*last;
  char *values;
  const char *entry = node->txt;
  int count = 1;
  if(!entry || !*entry) {
    ctx->set_error(ctx,400,"failed to parse dimension values: none supplied");
    return;
  }
  dimension = (mapcache_dimension_intervals*)dim;
  values = apr_pstrdup(ctx->pool,entry);

  for(key=values; *key; key++) if(*key == ',') count++;

  dimension->intervals = (mapcache_interval*)apr_pcalloc(ctx->pool,count*sizeof(mapcache_interval));

  for (key = apr_strtok(values, ",", &last); key != NULL;
       key = apr_strtok(NULL, ",", &last)) {
    char *endptr;
    mapcache_interval *interval = &dimension->intervals[dimension->nintervals];
    interval->start = strtod(key,&endptr);
    if(*endptr != '/') {
      ctx->set_error(ctx,400,"failed to parse min dimension value \"%s\" in \"%s\" for dimension %s",key,entry,dim->name);
      return;
    }
    key = endptr+1;

    interval->end = strtod(key,&endptr);
    if(*endptr != '/') {
      ctx->set_error(ctx,400,"failed to parse max dimension value \"%s\" in \"%s\" for dimension %s",key,entry,dim->name);
      return;
    }
    key = endptr+1;
    interval->resolution = strtod(key,&endptr);
    if(*endptr != '\0') {
      ctx->set_error(ctx,400,"failed to parse resolution dimension value \"%s\" in \"%s\" for dimension %s",key,entry,dim->name);
      return;
    }
    dimension->nintervals++;
  }

  if(!dimension->nintervals) {
    ctx->set_error(ctx, 400, "<dimension> \"%s\" has no intervals",dim->name);
    return;
  }
}

static int _mapcache_dimension_regex_validate(mapcache_context *ctx, mapcache_dimension *dim, char **value)
{
  mapcache_dimension_regex *dimension = (mapcache_dimension_regex*)dim;
#ifdef USE_PCRE
  int ovector[30];
  int rc = pcre_exec(dimension->pcregex,NULL,*value,strlen(*value),0,0,ovector,30);
  if(rc>0)
    return MAPCACHE_SUCCESS;
#else
  if(!regexec(dimension->regex,*value,0,0,0)) {
    return MAPCACHE_SUCCESS;
  }
#endif
  return MAPCACHE_FAILURE;
}

static apr_array_header_t* _mapcache_dimension_regex_print(mapcache_context *ctx, mapcache_dimension *dim)
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
  const char *entry = node->txt;
  if(!entry || !*entry) {
    ctx->set_error(ctx,400,"failed to parse dimension regex: none supplied");
    return;
  }
  dimension = (mapcache_dimension_regex*)dim;
  dimension->regex_string = apr_pstrdup(ctx->pool,entry);
#ifdef USE_PCRE
  {
    const char *pcre_err;
    int pcre_offset;
    dimension->pcregex = pcre_compile(entry,0,&pcre_err, &pcre_offset,0);
    if(!dimension->pcregex) {
      ctx->set_error(ctx,400,"failed to compile regular expression \"%s\" for dimension \"%s\": %s",
                     entry,dim->name,pcre_err);
      return;
    }
  }
#else
  {
    int rc = regcomp(dimension->regex, entry, REG_EXTENDED);
    if(rc) {
      char errmsg[200];
      regerror(rc,dimension->regex,errmsg,200);
      ctx->set_error(ctx,400,"failed to compile regular expression \"%s\" for dimension \"%s\": %s",
                     entry,dim->name,errmsg);
      return;
    }
  }
#endif

}

static int _mapcache_dimension_values_validate(mapcache_context *ctx, mapcache_dimension *dim, char **value)
{
  int i;
  mapcache_dimension_values *dimension = (mapcache_dimension_values*)dim;
  for(i=0; i<dimension->nvalues; i++) {
    if(dimension->case_sensitive) {
      if(!strcmp(*value,dimension->values[i]))
        return MAPCACHE_SUCCESS;
    } else {
      if(!strcasecmp(*value,dimension->values[i]))
        return MAPCACHE_SUCCESS;
    }
  }
  return MAPCACHE_FAILURE;
}

static apr_array_header_t* _mapcache_dimension_values_print(mapcache_context *ctx, mapcache_dimension *dim)
{
  mapcache_dimension_values *dimension = (mapcache_dimension_values*)dim;
  apr_array_header_t *ret = apr_array_make(ctx->pool,dimension->nvalues,sizeof(char*));
  int i;
  for(i=0; i<dimension->nvalues; i++) {
    APR_ARRAY_PUSH(ret,char*) = apr_pstrdup(ctx->pool,dimension->values[i]);
  }
  return ret;
}


static void _mapcache_dimension_values_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  int count = 1;
  mapcache_dimension_values *dimension;
  const char *case_sensitive;
  char *key,*last;
  char *values;
  const char *entry = node->txt;
  if(!entry || !*entry) {
    ctx->set_error(ctx,400,"failed to parse dimension values: none supplied");
    return;
  }

  dimension = (mapcache_dimension_values*)dim;
  case_sensitive = ezxml_attr(node,"case_sensitive");
  if(case_sensitive && !strcasecmp(case_sensitive,"true")) {
    dimension->case_sensitive = 1;
  }

  values = apr_pstrdup(ctx->pool,entry);
  for(key=values; *key; key++) if(*key == ',') count++;

  dimension->values = (char**)apr_pcalloc(ctx->pool,count*sizeof(char*));

  for (key = apr_strtok(values, ",", &last); key != NULL;
       key = apr_strtok(NULL, ",", &last)) {
    dimension->values[dimension->nvalues]=key;
    dimension->nvalues++;
  }
  if(!dimension->nvalues) {
    ctx->set_error(ctx, 400, "<dimension> \"%s\" has no values",dim->name);
    return;
  }
}

mapcache_dimension* mapcache_dimension_values_create(apr_pool_t *pool)
{
  mapcache_dimension_values *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_values));
  dimension->dimension.type = MAPCACHE_DIMENSION_VALUES;
  dimension->nvalues = 0;
  dimension->dimension.validate = _mapcache_dimension_values_validate;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_values_parse_xml;
  dimension->dimension.print_ogc_formatted_values = _mapcache_dimension_values_print;
  return (mapcache_dimension*)dimension;
}

mapcache_dimension* mapcache_dimension_time_create(apr_pool_t *pool)
{
  mapcache_dimension_time *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_time));
  dimension->dimension.type = MAPCACHE_DIMENSION_TIME;
  dimension->nintervals = 0;
//   dimension->dimension.validate = _mapcache_dimension_time_validate;
//   dimension->dimension.parse = _mapcache_dimension_time_parse;
//   dimension->dimension.print_ogc_formatted_values = _mapcache_dimension_time_print;
  return (mapcache_dimension*)dimension;
}

mapcache_dimension* mapcache_dimension_intervals_create(apr_pool_t *pool)
{
  mapcache_dimension_intervals *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_intervals));
  dimension->dimension.type = MAPCACHE_DIMENSION_INTERVALS;
  dimension->nintervals = 0;
  dimension->dimension.validate = _mapcache_dimension_intervals_validate;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_intervals_parse_xml;
  dimension->dimension.print_ogc_formatted_values = _mapcache_dimension_intervals_print;
  return (mapcache_dimension*)dimension;
}
mapcache_dimension* mapcache_dimension_regex_create(apr_pool_t *pool)
{
  mapcache_dimension_regex *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_regex));
  dimension->dimension.type = MAPCACHE_DIMENSION_REGEX;
#ifndef USE_PCRE
  dimension->regex = (regex_t*)apr_pcalloc(pool, sizeof(regex_t));
#endif
  dimension->dimension.validate = _mapcache_dimension_regex_validate;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_regex_parse_xml;
  dimension->dimension.print_ogc_formatted_values = _mapcache_dimension_regex_print;
  return (mapcache_dimension*)dimension;
}

#ifdef USE_SQLITE

struct sqlite_dimension_conn {
  sqlite3 *handle;
  sqlite3_stmt **prepared_statements;
  int n_statements;
};

void mapcache_sqlite_dimension_connection_constructor(mapcache_context *ctx, void **conn_, void *params, apr_pool_t *pool)
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

void mapcache_sqlite_dimension_connection_destructor(void *conn_, apr_pool_t *pool)
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

static mapcache_pooled_connection* _sqlite_time_dimension_get_conn(mapcache_context *ctx, mapcache_timedimension_sqlite *dim) {
  mapcache_pooled_connection *pc = mapcache_connection_pool_get_connection(ctx,dim->timedimension.key,
        mapcache_sqlite_dimension_connection_constructor,
        mapcache_sqlite_dimension_connection_destructor, dim->dbfile);
  return pc;
}
static mapcache_pooled_connection* _sqlite_dimension_get_conn(mapcache_context *ctx, mapcache_dimension_sqlite *dim) {
  mapcache_pooled_connection *pc = mapcache_connection_pool_get_connection(ctx,dim->dimension.name,
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


static int _mapcache_dimension_sqlite_validate(mapcache_context *ctx, mapcache_dimension *dim, char **value)
{
  mapcache_dimension_sqlite *dimension = (mapcache_dimension_sqlite*)dim;
  struct sqlite_dimension_conn *conn = NULL;
  int sqliteret,paramidx,ret=MAPCACHE_FAILURE;
  mapcache_pooled_connection *pc;
  pc = _sqlite_dimension_get_conn(ctx,dimension);
  if (GC_HAS_ERROR(ctx)) {
    return ret;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(2,sizeof(sqlite3_stmt*));
    conn->n_statements = 2;
  }
  if(!conn->prepared_statements[0]) {
    sqliteret = sqlite3_prepare_v2(conn->handle, dimension->validate_query, -1, &conn->prepared_statements[0], NULL);
    if(sqliteret != SQLITE_OK) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      goto cleanup;
    }
  }
  
  paramidx = sqlite3_bind_parameter_index(conn->prepared_statements[0], ":dim");
  if (paramidx) {
    sqliteret = sqlite3_bind_text(conn->prepared_statements[0], paramidx, *value, -1, SQLITE_STATIC);
    if(sqliteret != SQLITE_OK) {
      ctx->set_error(ctx,400, "sqlite dimension failed to bind :dim : %s", sqlite3_errmsg(conn->handle));
      goto cleanup;
    }
  }
  do {
    sqliteret = sqlite3_step(conn->prepared_statements[0]);
    if (sqliteret != SQLITE_DONE && sqliteret != SQLITE_ROW && sqliteret != SQLITE_BUSY && sqliteret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on query : %s (%d)", sqlite3_errmsg(conn->handle), sqliteret);
      goto cleanup;
    }
    if(sqliteret == SQLITE_ROW) {
      const char* dim_modified = (const char*) sqlite3_column_text(conn->prepared_statements[0], 0);
      if(strcmp(dim_modified, *value)) {
        *value = apr_pstrdup(ctx->pool, dim_modified);
      }
      ret = MAPCACHE_SUCCESS;
    }
  } while (sqliteret == SQLITE_ROW || sqliteret == SQLITE_BUSY || sqliteret == SQLITE_LOCKED);

cleanup:
  if(conn->prepared_statements[0]) {
    sqlite3_reset(conn->prepared_statements[0]);
  }
  _sqlite_dimension_release_conn(ctx,pc);
      
  return ret;
}

static apr_array_header_t* _mapcache_dimension_sqlite_print(mapcache_context *ctx, mapcache_dimension *dim)
{
  mapcache_dimension_sqlite *dimension = (mapcache_dimension_sqlite*)dim;
  struct sqlite_dimension_conn *conn = NULL;
  int sqliteret;
  apr_array_header_t *ret = apr_array_make(ctx->pool,0,sizeof(char*));
  mapcache_pooled_connection *pc;
  pc = _sqlite_dimension_get_conn(ctx,dimension);
  if (GC_HAS_ERROR(ctx)) {
    return ret;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(2,sizeof(sqlite3_stmt*));
    conn->n_statements = 2;
  }
 
  if(!conn->prepared_statements[1]) {
    sqliteret = sqlite3_prepare_v2(conn->handle, dimension->list_query, -1, &conn->prepared_statements[1], NULL);
    if(sqliteret != SQLITE_OK) {
      ctx->set_error(ctx, 500, "sqlite dimension backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      goto cleanup;
    }
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
    dimension->validate_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"sqlite dimension \"%s\" has no <validate_query> node", dim->name);
    return;
  }
  child = ezxml_child(node,"list_query");
  if(child) {
    dimension->list_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"sqlite dimension \"%s\" has no <list_query> node", dim->name);
    return;
  }
  
}


static void _bind_sqlite_timedimension_params(mapcache_context *ctx, sqlite3_stmt *stmt,
        sqlite3 *handle, mapcache_tileset *tileset, mapcache_grid *grid, mapcache_extent *extent,
        time_t start, time_t end)
{
  int paramidx,ret;
  paramidx = sqlite3_bind_parameter_index(stmt, ":tileset");
  if (paramidx) {
    ret = sqlite3_bind_text(stmt, paramidx, tileset->name, -1, SQLITE_STATIC);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx,400, "failed to bind :tileset: %s", sqlite3_errmsg(handle));
      return;
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

apr_array_header_t *_mapcache_timedimension_sqlite_get_entries(mapcache_context *ctx, mapcache_timedimension *dim,
        mapcache_tileset *tileset, mapcache_grid *grid, mapcache_extent *extent, time_t start, time_t end) {
  mapcache_timedimension_sqlite *sdim = (mapcache_timedimension_sqlite*)dim;
  int ret;
  apr_array_header_t *time_ids = NULL;
  mapcache_pooled_connection *pc;
  struct sqlite_dimension_conn *conn;
  pc = _sqlite_time_dimension_get_conn(ctx,sdim);
  if (GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  conn = pc->connection;
  if(!conn->prepared_statements) {
    conn->prepared_statements = calloc(1,sizeof(sqlite3_stmt*));
    conn->n_statements = 1;
  }
  if(!conn->prepared_statements[0]) {
    ret = sqlite3_prepare_v2(conn->handle, sdim->query, -1, &conn->prepared_statements[0], NULL);
    if(ret != SQLITE_OK) {
      ctx->set_error(ctx, 500, "time sqlite backend failed on preparing query: %s", sqlite3_errmsg(conn->handle));
      _sqlite_dimension_release_conn(ctx, pc);
      return NULL;
    }
  }
  
  _bind_sqlite_timedimension_params(ctx,conn->prepared_statements[0],conn->handle,tileset,grid,extent,start,end);
  if(GC_HAS_ERROR(ctx)) {
    _sqlite_dimension_release_conn(ctx, pc);
    return NULL;
  }
  
  time_ids = apr_array_make(ctx->pool,0,sizeof(char*));
  do {
    ret = sqlite3_step(conn->prepared_statements[0]);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite backend failed on timedimension query : %s (%d)", sqlite3_errmsg(conn->handle), ret);
      _sqlite_dimension_release_conn(ctx, pc);
      return NULL;
    }
    if(ret == SQLITE_ROW) {
      const char* time_id = (const char*) sqlite3_column_text(conn->prepared_statements[0], 0);
      APR_ARRAY_PUSH(time_ids,char*) = apr_pstrdup(ctx->pool,time_id);
    }
  } while (ret == SQLITE_ROW || ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  sqlite3_reset(conn->prepared_statements[0]);
  _sqlite_dimension_release_conn(ctx, pc);
  return time_ids;
}

apr_array_header_t *_mapcache_timedimension_sqlite_get_all_entries(mapcache_context *ctx, mapcache_timedimension *dim,
        mapcache_tileset *tileset) {
  return _mapcache_timedimension_sqlite_get_entries(ctx,dim,tileset,NULL,NULL,0,INT_MAX);
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
void _mapcache_timedimension_sqlite_parse_xml(mapcache_context *ctx, mapcache_timedimension *dim, ezxml_t node) {
  mapcache_timedimension_sqlite *sdim = (mapcache_timedimension_sqlite*)dim;
  ezxml_t child;
  
  child = ezxml_child(node,"dbfile");
  if(child && child->txt && *child->txt) {
    sdim->dbfile = apr_pstrdup(ctx->pool,child->txt);
  } else {
    ctx->set_error(ctx,400,"no <dbfile> entry for <timedimension> %s",dim->key);
    return;
  }
  child = ezxml_child(node,"query");
  if(child && child->txt && *child->txt) {
    sdim->query = apr_pstrdup(ctx->pool,child->txt);
  } else {
    ctx->set_error(ctx,400,"no <query> entry for <timedimension> %s",dim->key);
    return;
  }
}
#endif

char *mapcache_ogc_strptime(const char *value, struct tm *ts, mapcache_time_interval_t *ti) {
  memset (ts, '\0', sizeof (*ts));
  char *valueptr;
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

apr_array_header_t* mapcache_timedimension_get_entries_for_value(mapcache_context *ctx, mapcache_timedimension *timedimension,
        mapcache_tileset *tileset, mapcache_grid *grid, mapcache_extent *extent, const char *value) {
  /* look if supplied value is a predefined key */
  /* split multiple values, loop */

  /* extract start and end values */
  struct tm tm_start,tm_end;
  time_t start,end;
  mapcache_time_interval_t tis,tie;
  char *valueptr = (char*)value;
  valueptr = mapcache_ogc_strptime(value,&tm_start,&tis);
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
  end = timegm(&tm_end);
  start = timegm(&tm_start);
  if(difftime(start,end) == 0) {
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
    end = timegm(&tm_end);
  }

  return timedimension->get_entries_for_interval(ctx,timedimension,tileset,grid,extent,start,end);
  /* end loop */
}

mapcache_dimension* mapcache_dimension_sqlite_create(apr_pool_t *pool)
{
#ifdef USE_SQLITE
  mapcache_dimension_sqlite *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_sqlite));
  dimension->dimension.type = MAPCACHE_DIMENSION_SQLITE;
  dimension->dbfile = NULL;
  dimension->dimension.validate = _mapcache_dimension_sqlite_validate;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_sqlite_parse_xml;
  dimension->dimension.print_ogc_formatted_values = _mapcache_dimension_sqlite_print;
  return (mapcache_dimension*)dimension;
#else
  return NULL;
#endif
}


#ifdef USE_SQLITE
mapcache_timedimension* mapcache_timedimension_sqlite_create(apr_pool_t *pool) {
  mapcache_timedimension_sqlite *dim = apr_pcalloc(pool, sizeof(mapcache_timedimension_sqlite));
  dim->timedimension.assembly_type = MAPCACHE_TIMEDIMENSION_ASSEMBLY_STACK;
  dim->timedimension.get_entries_for_interval = _mapcache_timedimension_sqlite_get_entries;
  dim->timedimension.get_all_entries = _mapcache_timedimension_sqlite_get_all_entries;
  dim->timedimension.configuration_parse_xml = _mapcache_timedimension_sqlite_parse_xml;
  return (mapcache_timedimension*)dim;
}
#endif
/* vim: ts=2 sts=2 et sw=2
*/
