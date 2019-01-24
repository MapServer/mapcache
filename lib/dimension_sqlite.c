/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: SQLite dimension support
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
#ifdef USE_SQLITE
#include <sqlite3.h>
#include <float.h>

typedef struct mapcache_dimension_sqlite mapcache_dimension_sqlite;

struct mapcache_dimension_sqlite {
  mapcache_dimension dimension;
  char *dbfile;
  char *get_values_for_entry_query;
  char *get_all_values_query;
};

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
    goto cleanup;
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
    goto cleanup;
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

apr_array_header_t* _mapcache_dimension_sqlite_get_entries_for_time_range(mapcache_context *ctx, mapcache_dimension *dim, const char *dim_value,
        time_t start, time_t end, mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  mapcache_dimension_sqlite *sdim = (mapcache_dimension_sqlite*)dim;
  int ret;
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
  
  _bind_sqlite_dimension_time_params(ctx,conn->prepared_statements[0],conn->handle,dim_value,tileset,grid,extent,start,end);
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
    if (ret == SQLITE_ROW) {
      const char *time_id = (const char *)sqlite3_column_text(conn->prepared_statements[0], 0);
      APR_ARRAY_PUSH(time_ids, char *) = apr_pstrdup(ctx->pool, time_id);
    }
  } while (ret == SQLITE_ROW || ret == SQLITE_BUSY || ret == SQLITE_LOCKED);

  sqlite3_reset(conn->prepared_statements[0]);
  _sqlite_dimension_release_conn(ctx, pc);
  return time_ids;
}

/*
apr_array_header_t* _mapcache_dimension_time_get_all_entries(mapcache_context *ctx, mapcache_dimension *dim,
        mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  mapcache_dimension_time *tdim = (mapcache_dimension_time*)dim;
  time_t all[2] = {0,INT_MAX};
  return _mapcache_dimension_time_get_entries(ctx,tdim,NULL,tileset,extent,grid,all,1);
}
*/
#endif

mapcache_dimension* mapcache_dimension_sqlite_create(mapcache_context *ctx, apr_pool_t *pool)
{
#ifdef USE_SQLITE
  mapcache_dimension_sqlite *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_sqlite));
  dimension->dimension.type = MAPCACHE_DIMENSION_SQLITE;
  dimension->dbfile = NULL;
  dimension->dimension._get_entries_for_value = _mapcache_dimension_sqlite_get_entries_for_value;
  dimension->dimension._get_entries_for_time_range = _mapcache_dimension_sqlite_get_entries_for_time_range;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_sqlite_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_sqlite_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_sqlite_get_all_entries;
  return (mapcache_dimension*)dimension;
#else
  ctx->set_error(ctx,400,"Sqlite dimension support requires SQLITE support to be built in");
  return NULL;
#endif
}
