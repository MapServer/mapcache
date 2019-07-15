/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: PostgreSQL dimension support
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
#include <apr_hash.h>
#include <float.h>
#ifdef USE_POSTGRESQL
#include <libpq-fe.h>

typedef struct mapcache_dimension_postgresql mapcache_dimension_postgresql;

struct mapcache_dimension_postgresql {
  mapcache_dimension dimension;
  char *dbconnection;
  char *get_values_for_entry_query;
  char *get_all_values_query;
  apr_hash_t  *get_values_indexes;
  apr_hash_t  *get_all_indexes;
};

struct postgresql_dimension_conn {
  PGconn      *pgconn;     /* Connection to database */
};

/* lookup occurences of "param" in qstring, and replace them with $idx if found. returns the number of times
 * param was found. replacement is done in place because the replacement string is known to be shorter than the original*/
static int qparam(mapcache_context *ctx, char *qstring, const char *param, int idx) {
  int nFound = 0;
  char *didx=NULL;
  while(1) {
    char *sidx = strstr(qstring,param);
    char *endstring;
    //printf("lookup %s iter %d: string %s\n",param,nFound,qstring);
    if(!sidx) {
      return nFound;
    }
    nFound++;
    if(!didx) {
      didx = apr_psprintf(ctx->pool,"$%d", idx);
    }
    strcpy(sidx,didx);
    endstring = apr_pstrdup(ctx->pool,sidx+strlen(param));
    strcpy(sidx+strlen(didx),endstring);
  }
}
#define INT2VOIDP(i) (void*)(uintptr_t)(i)

static void parse_queries(mapcache_context *ctx, mapcache_dimension_postgresql *dim) {
  const char *keys[9] = {":tileset",":dim",":gridsrs",":minx",":maxx",":miny",":maxy",":start_timestamp",":end_timestamp"};
  int i;
  int gaidx=1,gvidx=1;
  dim->get_all_indexes = apr_hash_make(ctx->pool);
  dim->get_values_indexes = apr_hash_make(ctx->pool);
  for(i=0;i<9;i++) {
    if(qparam(ctx,dim->get_all_values_query,keys[i],gaidx)) {
      apr_hash_set(dim->get_all_indexes,keys[i],APR_HASH_KEY_STRING,INT2VOIDP(gaidx));
      gaidx++;
    }
    if(qparam(ctx,dim->get_values_for_entry_query,keys[i],gvidx)) {
      apr_hash_set(dim->get_values_indexes,keys[i],APR_HASH_KEY_STRING,INT2VOIDP(gvidx));
      gvidx++;
    }
  }
}

#define VOIDP2INT(i) (int)(uintptr_t)(i)
static void _mapcache_dimension_postgresql_bind_parameters(mapcache_context *ctx, apr_hash_t *param_indexes,
       char *dim_value,
       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid,
       time_t start, time_t end,
       int *nParams, char ***paramValues, int **paramLengths, int **paramFormats) {

  int paramidx;
  *nParams = apr_hash_count(param_indexes);
  *paramValues = apr_pcalloc(ctx->pool, *nParams*sizeof(char*));
  *paramLengths = apr_pcalloc(ctx->pool, *nParams*sizeof(int));
  *paramFormats = apr_pcalloc(ctx->pool, *nParams*sizeof(int));

  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":dim",APR_HASH_KEY_STRING));
  if (paramidx) {
    paramidx-=1;
    (*paramValues)[paramidx] = dim_value;
    (*paramLengths)[paramidx] = strlen(dim_value);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":tileset",APR_HASH_KEY_STRING));
  if (paramidx) {
    paramidx-=1;
    //printf("set tileset at %d to %s\n",paramidx,tileset->name);
    (*paramValues)[paramidx] = tileset->name;
    (*paramLengths)[paramidx] = strlen(tileset->name);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":gridsrs",APR_HASH_KEY_STRING));
  if (paramidx) {
    paramidx-=1;
    (*paramValues)[paramidx] = grid->srs;
    (*paramLengths)[paramidx] = strlen(grid->srs);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":minx",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%f",extent?extent->minx:-DBL_MAX);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":miny",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%f",extent?extent->miny:-DBL_MAX);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":maxx",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%f",extent?extent->maxx:DBL_MAX);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":maxy",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%f",extent?extent->maxy:DBL_MAX);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":start_timestamp",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%ld",start);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
  paramidx = VOIDP2INT(apr_hash_get(param_indexes,":end_timestamp",APR_HASH_KEY_STRING));
  if (paramidx) {
    char *buf = apr_psprintf(ctx->pool,"%ld",end);
    paramidx-=1;
    (*paramValues)[paramidx] = buf;
    (*paramLengths)[paramidx] = strlen(buf);
    (*paramFormats)[paramidx] = 0;
  }
}

static void prepare_query(mapcache_context *ctx, PGconn *conn, char *stmt_name, char *qstring, apr_hash_t *param_indexes) {

  /*
  apr_hash_index_t *param_index = apr_hash_first(ctx->pool,param_indexes);
  while(param_index) {
    const void *key;
    apr_ssize_t keylen;
    int idx;
    apr_hash_this(param_index,&key,&keylen,(void**)&idx);
    param_index = apr_hash_next(param_index);
  }
  */
  PGresult *res = PQprepare(conn,stmt_name,qstring,apr_hash_count(param_indexes), NULL);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    ctx->set_error(ctx,500,"prepare query: %s",PQerrorMessage(conn));
    PQclear(res);
    return;
  }
  PQclear(res);
}

void mapcache_postgresql_dimension_connection_constructor(mapcache_context *ctx, void **conn_, void *params)
{
  mapcache_dimension_postgresql *dim = (mapcache_dimension_postgresql*) params;
  struct postgresql_dimension_conn *conn = calloc(1, sizeof (struct postgresql_dimension_conn));
  *conn_ = conn;
  conn->pgconn = PQconnectdb(dim->dbconnection);
  /* Check to see that the backend connection was successfully made */
  if (PQstatus(conn->pgconn) != CONNECTION_OK) {
    ctx->set_error(ctx, 500, "failed to open postgresql connection: %s", PQerrorMessage(conn->pgconn));
    PQfinish(conn->pgconn);
    *conn_ = NULL;
    return;
  }
  prepare_query(ctx,conn->pgconn, "get_value", dim->get_values_for_entry_query, dim->get_values_indexes);
  if(GC_HAS_ERROR(ctx)) {
    PQfinish(conn->pgconn);
    *conn_ = NULL;
    return;
  }
  prepare_query(ctx,conn->pgconn, "get_all", dim->get_all_values_query, dim->get_all_indexes);
  if(GC_HAS_ERROR(ctx)) {
    PQfinish(conn->pgconn);
    *conn_ = NULL;
    return;
  }
}

void mapcache_postgresql_dimension_connection_destructor(void *conn_)
{
  struct postgresql_dimension_conn *conn = (struct postgresql_dimension_conn*) conn_;
  PQfinish(conn->pgconn);
  free(conn);
}

static mapcache_pooled_connection* _postgresql_dimension_get_conn(mapcache_context *ctx, mapcache_tileset *tileset, mapcache_dimension_postgresql *dim) {
  mapcache_dimension *pdim = (mapcache_dimension*)dim;
  char *conn_key = apr_pstrcat(ctx->pool,"dim_",tileset?tileset->name:"","_",pdim->name,NULL);
  mapcache_pooled_connection *pc = mapcache_connection_pool_get_connection(ctx,conn_key,
        mapcache_postgresql_dimension_connection_constructor,
        mapcache_postgresql_dimension_connection_destructor, dim);
  return pc;
}

static void _postgresql_dimension_release_conn(mapcache_context *ctx, mapcache_pooled_connection *pc)
{
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
  } else {
    mapcache_connection_pool_release_connection(ctx,pc);
  }
}

apr_array_header_t* _mapcache_dimension_postgresql_get_entries_for_time_range(mapcache_context *ctx, mapcache_dimension *dim, const char *dim_value,
        time_t start, time_t end, mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  mapcache_dimension_postgresql *sdim = (mapcache_dimension_postgresql*)dim;
  PGresult *res;
  apr_array_header_t *time_ids = NULL;
  mapcache_pooled_connection *pc;
  struct postgresql_dimension_conn *conn;
  int nParams, *paramLengths,*paramFormats,i;
  char **paramValues;

  pc = _postgresql_dimension_get_conn(ctx,tileset,sdim);
  if (GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  conn = pc->connection;
  _mapcache_dimension_postgresql_bind_parameters(ctx,sdim->get_values_indexes,(char*)dim_value,tileset,extent,grid,start,end,&nParams,&paramValues,&paramLengths,&paramFormats);
  if(GC_HAS_ERROR(ctx)) {
    _postgresql_dimension_release_conn(ctx, pc);
    return NULL;
  }

  res = PQexecPrepared(conn->pgconn,"get_value",nParams,(const char *const*)paramValues,paramLengths,paramFormats,0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    ctx->set_error(ctx, 500, "postgresql query: %s", PQerrorMessage(conn->pgconn));
    PQclear(res);
    _postgresql_dimension_release_conn(ctx, pc);
    return NULL;
  }

  time_ids = apr_array_make(ctx->pool,0,sizeof(char*));
  for(i=0;i<PQntuples(res);i++) {
    APR_ARRAY_PUSH(time_ids, char *) = apr_pstrdup(ctx->pool, PQgetvalue(res,i,0));
  }
  PQclear(res);
  _postgresql_dimension_release_conn(ctx, pc);
  return time_ids;
}

static apr_array_header_t* _mapcache_dimension_postgresql_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dim, const char *value,
     mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  return _mapcache_dimension_postgresql_get_entries_for_time_range(ctx,dim,value,0,0,tileset,extent,grid);
}

static apr_array_header_t* _mapcache_dimension_postgresql_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
       mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  mapcache_dimension_postgresql *sdim = (mapcache_dimension_postgresql*)dim;
  PGresult *res;
  apr_array_header_t *time_ids = NULL;
  mapcache_pooled_connection *pc;
  struct postgresql_dimension_conn *conn;
  int nParams, *paramLengths,*paramFormats,i;
  char **paramValues;

  pc = _postgresql_dimension_get_conn(ctx,tileset,sdim);
  if (GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  conn = pc->connection;
  _mapcache_dimension_postgresql_bind_parameters(ctx,sdim->get_all_indexes,NULL,tileset,extent,grid,0,0,&nParams,&paramValues,&paramLengths,&paramFormats);
  if(GC_HAS_ERROR(ctx)) {
    _postgresql_dimension_release_conn(ctx, pc);
    return NULL;
  }

  res = PQexecPrepared(conn->pgconn,"get_all",nParams,(const char *const*)paramValues,paramLengths,paramFormats,0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    ctx->set_error(ctx, 500, "postgresql query: %s", PQerrorMessage(conn->pgconn));
    PQclear(res);
    _postgresql_dimension_release_conn(ctx, pc);
    return NULL;
  }

  //printf("got %d results\n",PQntuples(res));
  time_ids = apr_array_make(ctx->pool,0,sizeof(char*));
  for(i=0;i<PQntuples(res);i++) {
    APR_ARRAY_PUSH(time_ids, char *) = apr_pstrdup(ctx->pool, PQgetvalue(res,i,0));
  }
  PQclear(res);
  _postgresql_dimension_release_conn(ctx, pc);
  return time_ids;

}

static void _mapcache_dimension_postgresql_parse_xml(mapcache_context *ctx, mapcache_dimension *dim,
    ezxml_t node)
{
  mapcache_dimension_postgresql *dimension;
  ezxml_t child;

  dimension = (mapcache_dimension_postgresql*)dim;

  child = ezxml_child(node,"connection");
  if(child) {
    dimension->dbconnection = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"postgresql dimension \"%s\" has no <connection> node", dim->name);
    return;
  }
  child = ezxml_child(node,"validate_query");
  if(child) {
    dimension->get_values_for_entry_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"postgresql dimension \"%s\" has no <validate_query> node", dim->name);
    return;
  }
  child = ezxml_child(node,"list_query");
  if(child) {
    dimension->get_all_values_query = apr_pstrdup(ctx->pool, child->txt);
  } else {
    ctx->set_error(ctx,400,"postgresql dimension \"%s\" has no <list_query> node", dim->name);
    return;
  }
  parse_queries(ctx,dimension);
  //printf("q1: %s\n",dimension->get_all_values_query);
  //printf("q2: %s\n",dimension->get_values_for_entry_query);
}
#endif


mapcache_dimension* mapcache_dimension_postgresql_create(mapcache_context *ctx, apr_pool_t *pool)
{
#ifdef USE_POSTGRESQL
  mapcache_dimension_postgresql *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_postgresql));
  dimension->dimension.type = MAPCACHE_DIMENSION_POSTGRESQL;
  dimension->dbconnection = NULL;
  dimension->dimension._get_entries_for_value = _mapcache_dimension_postgresql_get_entries_for_value;
  dimension->dimension._get_entries_for_time_range = _mapcache_dimension_postgresql_get_entries_for_time_range;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_postgresql_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_postgresql_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_postgresql_get_all_entries;
  return (mapcache_dimension*)dimension;
#else
  ctx->set_error(ctx,400,"postgresql dimension support requires POSTGRESQL support to be built in");
  return NULL;
#endif
}
