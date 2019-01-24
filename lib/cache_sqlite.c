/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: sqlite cache backend
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
#ifdef USE_SQLITE

#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <apr_reslist.h>
#include <apr_hash.h>
#ifdef APR_HAS_THREADS
#include <apr_thread_mutex.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#include <sqlite3.h>

/**\class mapcache_cache_sqlite
 * \brief a mapcache_cache on a filesytem
 * \implements mapcache_cache
 */
typedef struct mapcache_cache_sqlite mapcache_cache_sqlite;
typedef struct mapcache_cache_sqlite_stmt mapcache_cache_sqlite_stmt;

struct mapcache_cache_sqlite_stmt {
  char *sql;
};

struct sqlite_conn;

struct mapcache_cache_sqlite {
  mapcache_cache cache;
  char *dbfile;
  mapcache_cache_sqlite_stmt create_stmt;
  mapcache_cache_sqlite_stmt exists_stmt;
  mapcache_cache_sqlite_stmt get_stmt;
  mapcache_cache_sqlite_stmt set_stmt;
  mapcache_cache_sqlite_stmt delete_stmt;
  apr_table_t *pragmas;
  void (*bind_stmt)(mapcache_context *ctx, void *stmt, mapcache_cache_sqlite *cache, mapcache_tile *tile);
  int n_prepared_statements;
  int detect_blank;
  char *x_fmt,*y_fmt,*z_fmt,*inv_x_fmt,*inv_y_fmt,*div_x_fmt,*div_y_fmt,*inv_div_x_fmt,*inv_div_y_fmt;
  int count_x, count_y;
};


struct sqlite_conn_params {
  mapcache_cache_sqlite *cache;
  char *dbfile;
  int readonly;
};

struct sqlite_conn {
  sqlite3 *handle;
  int nstatements;
  sqlite3_stmt **prepared_statements;
};

#define SQLITE_CONN(pooled_connection) ((struct sqlite_conn*)((pooled_connection)->connection))

#define HAS_TILE_STMT_IDX 0
#define GET_TILE_STMT_IDX 1
#define SQLITE_SET_TILE_STMT_IDX 2
#define SQLITE_DEL_TILE_STMT_IDX 3
#define MBTILES_SET_EMPTY_TILE_STMT1_IDX 2
#define MBTILES_SET_EMPTY_TILE_STMT2_IDX 3
#define MBTILES_SET_TILE_STMT1_IDX 4
#define MBTILES_SET_TILE_STMT2_IDX 5
#define MBTILES_DEL_TILE_SELECT_STMT_IDX 6
#define MBTILES_DEL_TILE_STMT1_IDX 7
#define MBTILES_DEL_TILE_STMT2_IDX 8

static void _mapcache_cache_sqlite_filename_for_tile(mapcache_context *ctx, mapcache_cache_sqlite *dcache, mapcache_tile *tile, char **path);

static void _sqlite_set_pragmas(mapcache_context *ctx, mapcache_cache_sqlite* cache, struct sqlite_conn *conn)
{
  if (cache->pragmas && !apr_is_empty_table(cache->pragmas)) {
    const apr_array_header_t *elts = apr_table_elts(cache->pragmas);
    /* FIXME dynamically allocate this string */
    int i,ret;
    char *pragma_stmt;
    for (i = 0; i < elts->nelts; i++) {
      apr_table_entry_t entry = APR_ARRAY_IDX(elts, i, apr_table_entry_t);
      pragma_stmt = apr_psprintf(ctx->pool,"PRAGMA %s=%s",entry.key,entry.val);
      do {
        ret = sqlite3_exec(conn->handle, pragma_stmt, 0, 0, NULL);
        if (ret != SQLITE_OK && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
          break;
        }
      } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
      if (ret != SQLITE_OK) {
        ctx->set_error(ctx, 500, "failed to execute pragma statement %s",pragma_stmt);
        return;
      }
    }
  }
  return;
}



static void mapcache_sqlite_release_conn(mapcache_context *ctx, mapcache_pooled_connection *conn) {
  mapcache_connection_pool_release_connection(ctx,conn);
}

void mapcache_sqlite_connection_constructor(mapcache_context *ctx, void **conn_, void *params)
{
  int ret;
  int flags;  
  struct sqlite_conn_params *sq_params = (struct sqlite_conn_params*)params;
  struct sqlite_conn *conn = calloc(1, sizeof (struct sqlite_conn));
  *conn_ = conn;
  if(sq_params->readonly) {
    flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
  } else {
    flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_CREATE;
    mapcache_make_parent_dirs(ctx,sq_params->dbfile);
    GC_CHECK_ERROR(ctx);
  }
  ret = sqlite3_open_v2(sq_params->dbfile, &conn->handle, flags, NULL);
  if (ret != SQLITE_OK) {
    ctx->set_error(ctx,500,"sqlite backend failed to open db %s: %s", sq_params->dbfile, sqlite3_errmsg(conn->handle));
    return;
  }
  sqlite3_busy_timeout(conn->handle, 300000);
  do {
    ret = sqlite3_exec(conn->handle, sq_params->cache->create_stmt.sql, 0, 0, NULL);
    if (ret != SQLITE_OK && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      break;
    }
  } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  if (ret != SQLITE_OK) {
    ctx->set_error(ctx,500, "sqlite backend failed to create db schema on %s: %s", sq_params->dbfile, sqlite3_errmsg(conn->handle));
    sqlite3_close(conn->handle);
    return;
  }
  _sqlite_set_pragmas(ctx, sq_params->cache, conn);
  if(GC_HAS_ERROR(ctx)) {
    sqlite3_close(conn->handle);
    return;
  }
  conn->prepared_statements = calloc(sq_params->cache->n_prepared_statements,sizeof(sqlite3_stmt*));
  conn->nstatements = sq_params->cache->n_prepared_statements;
}

void mapcache_sqlite_connection_destructor(void *conn_)
{
  struct sqlite_conn *conn = (struct sqlite_conn*) conn_;
  int i;
  for(i=0; i<conn->nstatements; i++) {
    if(conn->prepared_statements[i]) {
      sqlite3_finalize(conn->prepared_statements[i]);
    }
  }
  free(conn->prepared_statements);
  sqlite3_close(conn->handle);
}

static mapcache_pooled_connection* mapcache_sqlite_get_conn(mapcache_context *ctx, mapcache_cache_sqlite *cache, mapcache_tile *tile, int readonly) {
  struct sqlite_conn_params params;
  mapcache_pooled_connection *pc;
  char *key;
  _mapcache_cache_sqlite_filename_for_tile(ctx,cache,tile,&params.dbfile);
  params.cache = cache;
  params.readonly = readonly;
  if(!strstr(cache->dbfile,"{")) {
    key = apr_pstrcat(ctx->pool,readonly?"ro_":"rw_",cache->cache.name,NULL);
  } else {
    key = apr_pstrcat(ctx->pool,readonly?"ro_":"rw_",params.dbfile,NULL);
  }
  pc = mapcache_connection_pool_get_connection(ctx,key,mapcache_sqlite_connection_constructor,mapcache_sqlite_connection_destructor,&params);
  return pc;
}


/**
 * \brief return sqlite db filename for given tile
 *
 * \param tile the tile to get the key from
 * \param path pointer to a char* that will contain the filename
 * \param r
 * \private \memberof mapcache_cache_sqlite
 */
static void _mapcache_cache_sqlite_filename_for_tile(mapcache_context *ctx, mapcache_cache_sqlite *dcache, mapcache_tile *tile, char **path)
{
  *path = dcache->dbfile;

  if(strstr(*path,"{")) {
    /*
     * generic template substitutions
     */
    if(strstr(*path,"{tileset}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}",
              tile->tileset->name);
    if(strstr(*path,"{grid}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}",
              tile->grid_link->grid->name);
    if(tile->dimensions && strstr(*path,"{dim")) {
      char *dimstring="";
      int i = tile->dimensions->nelts;
      while(i--) {
        mapcache_requested_dimension *entry = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
        const char *dimval = mapcache_util_str_sanitize(ctx->pool,entry->cached_value,"/.",'#');
        char *single_dim = apr_pstrcat(ctx->pool,"{dim:",entry->dimension->name,"}",NULL);
        dimstring = apr_pstrcat(ctx->pool,dimstring,"#",dimval,NULL);
        if(strstr(*path,single_dim)) {
          *path = mapcache_util_str_replace(ctx->pool,*path, single_dim, dimval);
        }
      }
      *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
    }
    
    
    while(strstr(*path,"{z}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
              apr_psprintf(ctx->pool,dcache->z_fmt,tile->z));
    
    if(dcache->count_x > 0) {
      while(strstr(*path,"{div_x}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{div_x}",
                apr_psprintf(ctx->pool,dcache->div_x_fmt,tile->x/dcache->count_x));
      while(strstr(*path,"{inv_div_x}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_div_x}",
                apr_psprintf(ctx->pool,dcache->inv_div_x_fmt,(tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1)/dcache->count_x));
      while(strstr(*path,"{x}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                apr_psprintf(ctx->pool,dcache->x_fmt,tile->x/dcache->count_x*dcache->count_x));
      while(strstr(*path,"{inv_x}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                apr_psprintf(ctx->pool,dcache->inv_x_fmt,(tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1)/dcache->count_x*dcache->count_x));
    }
    
    if(dcache->count_y > 0) {
      while(strstr(*path,"{div_y}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{div_y}",
                apr_psprintf(ctx->pool,dcache->div_y_fmt,tile->y/dcache->count_y));
      while(strstr(*path,"{inv_div_y}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_div_y}",
                apr_psprintf(ctx->pool,dcache->inv_div_y_fmt,(tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1)/dcache->count_y));
      while(strstr(*path,"{y}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                apr_psprintf(ctx->pool,dcache->y_fmt,tile->y/dcache->count_y*dcache->count_y));
      while(strstr(*path,"{inv_y}"))
        *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                apr_psprintf(ctx->pool,dcache->inv_y_fmt,(tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1)/dcache->count_y*dcache->count_y));
      
    }
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}



/**
 * \brief apply appropriate tile properties to the sqlite statement */
static void _bind_sqlite_params(mapcache_context *ctx, void *vstmt, mapcache_cache_sqlite *cache, mapcache_tile *tile)
{
  sqlite3_stmt *stmt = vstmt;
  int paramidx;
  /* tile->x */
  paramidx = sqlite3_bind_parameter_index(stmt, ":x");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->x);

  paramidx = sqlite3_bind_parameter_index(stmt, ":inv_x");
  if (paramidx)
    sqlite3_bind_int(stmt, paramidx, tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1);

  /* tile->y */
  paramidx = sqlite3_bind_parameter_index(stmt, ":y");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->y);

  paramidx = sqlite3_bind_parameter_index(stmt, ":inv_y");
  if (paramidx)
    sqlite3_bind_int(stmt, paramidx, tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1);

  /* tile->z */
  paramidx = sqlite3_bind_parameter_index(stmt, ":z");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->z);

  /* eventual dimensions */
  paramidx = sqlite3_bind_parameter_index(stmt, ":dim");
  if (paramidx) {
    if (tile->dimensions) {
      char *dim = mapcache_util_get_tile_dimkey(ctx, tile, NULL, NULL);
      sqlite3_bind_text(stmt, paramidx, dim, -1, SQLITE_STATIC);
    } else {
      sqlite3_bind_text(stmt, paramidx, "", -1, SQLITE_STATIC);
    }
  }

  /* grid */
  paramidx = sqlite3_bind_parameter_index(stmt, ":grid");
  if (paramidx) sqlite3_bind_text(stmt, paramidx, tile->grid_link->grid->name, -1, SQLITE_STATIC);

  /* tileset */
  paramidx = sqlite3_bind_parameter_index(stmt, ":tileset");
  if (paramidx) sqlite3_bind_text(stmt, paramidx, tile->tileset->name, -1, SQLITE_STATIC);

  /* tile blob data */
  paramidx = sqlite3_bind_parameter_index(stmt, ":data");
  if (paramidx) {
    int written = 0;
    if(cache->detect_blank) {
      if(!tile->raw_image) {
        tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
        GC_CHECK_ERROR(ctx);
      }
      if(mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
        char *buf = apr_palloc(ctx->pool, 5* sizeof(char));
        buf[0] = '#';
        memcpy(buf+1,tile->raw_image->data,4);
        written = 1;
        sqlite3_bind_blob(stmt, paramidx, buf, 5, SQLITE_STATIC);
      }
    }
    if(!written) {
      if (!tile->encoded_data) {
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        GC_CHECK_ERROR(ctx);
      }
      if (tile->encoded_data && tile->encoded_data->size) {
        sqlite3_bind_blob(stmt, paramidx, tile->encoded_data->buf, tile->encoded_data->size, SQLITE_STATIC);
      } else {
        sqlite3_bind_text(stmt, paramidx, "", -1, SQLITE_STATIC);
      }
    }
  }
}

static void _bind_mbtiles_params(mapcache_context *ctx, void *vstmt, mapcache_cache_sqlite *cache, mapcache_tile *tile)
{
  sqlite3_stmt *stmt = vstmt;
  int paramidx;
  paramidx = sqlite3_bind_parameter_index(stmt, ":x");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->x);

  /* tile->y */
  paramidx = sqlite3_bind_parameter_index(stmt, ":y");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->y);

  /* tile->y */
  paramidx = sqlite3_bind_parameter_index(stmt, ":z");
  if (paramidx) sqlite3_bind_int(stmt, paramidx, tile->z);

  /* mbtiles foreign key */
  paramidx = sqlite3_bind_parameter_index(stmt, ":key");
  if (paramidx) {
    char *key = apr_psprintf(ctx->pool,"%d-%d-%d",tile->x,tile->y,tile->z);
    sqlite3_bind_text(stmt, paramidx, key, -1, SQLITE_STATIC);
  }

  paramidx = sqlite3_bind_parameter_index(stmt, ":color");
  if (paramidx) {
    char *key;
    assert(tile->raw_image);    
    key = apr_psprintf(ctx->pool,"#%02x%02x%02x%02x",
                             tile->raw_image->data[0],
                             tile->raw_image->data[1],
                             tile->raw_image->data[2],
                             tile->raw_image->data[3]);
    sqlite3_bind_text(stmt, paramidx, key, -1, SQLITE_STATIC);
  }
  
  /* tile blob data */
  paramidx = sqlite3_bind_parameter_index(stmt, ":data");
  if (paramidx) {
    if (!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
    }
    if (tile->encoded_data && tile->encoded_data->size) {
      sqlite3_bind_blob(stmt, paramidx, tile->encoded_data->buf, tile->encoded_data->size, SQLITE_STATIC);
    } else {
      sqlite3_bind_text(stmt, paramidx, "", -1, SQLITE_STATIC);
    }
  }

}

static int _mapcache_cache_sqlite_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) pcache;
  mapcache_pooled_connection *pc;
  struct sqlite_conn *conn;
  sqlite3_stmt *stmt;
  int ret;
  pc = mapcache_sqlite_get_conn(ctx,cache,tile,1);
  if (GC_HAS_ERROR(ctx)) {
    if(pc) mapcache_sqlite_release_conn(ctx, pc);
    if(!tile->tileset->read_only && tile->tileset->source) {
      /* not an error in this case, as the db file may not have been created yet */
      ctx->clear_errors(ctx);
    }
    return MAPCACHE_FALSE;
  }
  conn = SQLITE_CONN(pc);
  stmt = conn->prepared_statements[HAS_TILE_STMT_IDX];
  if(!stmt) {
    sqlite3_prepare(conn->handle, cache->exists_stmt.sql, -1, &conn->prepared_statements[HAS_TILE_STMT_IDX], NULL);
    stmt = conn->prepared_statements[HAS_TILE_STMT_IDX];
  }
  cache->bind_stmt(ctx, stmt, cache, tile);
  ret = sqlite3_step(stmt);
  if (ret != SQLITE_DONE && ret != SQLITE_ROW) {
    ctx->set_error(ctx, 500, "sqlite backend failed on has_tile: %s", sqlite3_errmsg(conn->handle));
  }
  if (ret == SQLITE_DONE) {
    ret = MAPCACHE_FALSE;
  } else if (ret == SQLITE_ROW) {
    ret = MAPCACHE_TRUE;
  }
  sqlite3_reset(stmt);
  mapcache_sqlite_release_conn(ctx, pc);
  return ret;
}

static void _mapcache_cache_sqlite_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) pcache;
  mapcache_pooled_connection *pc;
  struct sqlite_conn *conn;
  sqlite3_stmt *stmt;
  int ret;
  pc = mapcache_sqlite_get_conn(ctx,cache,tile,0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);
  stmt = conn->prepared_statements[SQLITE_DEL_TILE_STMT_IDX];
  if(!stmt) {
    sqlite3_prepare(conn->handle, cache->delete_stmt.sql, -1, &conn->prepared_statements[SQLITE_DEL_TILE_STMT_IDX], NULL);
    stmt = conn->prepared_statements[SQLITE_DEL_TILE_STMT_IDX];
  }
  cache->bind_stmt(ctx, stmt, cache, tile);
  ret = sqlite3_step(stmt);
  if (ret != SQLITE_DONE && ret != SQLITE_ROW) {
    ctx->set_error(ctx, 500, "sqlite backend failed on delete: %s", sqlite3_errmsg(conn->handle));
  }
  sqlite3_reset(stmt);
  mapcache_sqlite_release_conn(ctx, pc);
}


static void _mapcache_cache_mbtiles_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) pcache;
  mapcache_pooled_connection *pc;
  struct sqlite_conn *conn;
  sqlite3_stmt *stmt1,*stmt2,*stmt3;
  int ret;
  const char *tile_id;
  size_t tile_id_size;
  pc = mapcache_sqlite_get_conn(ctx,cache,tile,0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);
  stmt1 = conn->prepared_statements[MBTILES_DEL_TILE_SELECT_STMT_IDX];
  stmt2 = conn->prepared_statements[MBTILES_DEL_TILE_STMT1_IDX];
  stmt3 = conn->prepared_statements[MBTILES_DEL_TILE_STMT2_IDX];
  if(!stmt1) {
    sqlite3_prepare(conn->handle, "select tile_id from map where tile_col=:x and tile_row=:y and zoom_level=:z",-1,&conn->prepared_statements[MBTILES_DEL_TILE_SELECT_STMT_IDX], NULL);
    sqlite3_prepare(conn->handle, "delete from map where tile_col=:x and tile_row=:y and zoom_level=:z", -1, &conn->prepared_statements[MBTILES_DEL_TILE_STMT1_IDX], NULL);
    sqlite3_prepare(conn->handle, "delete from images where tile_id=:foobar", -1, &conn->prepared_statements[MBTILES_DEL_TILE_STMT2_IDX], NULL);
    stmt1 = conn->prepared_statements[MBTILES_DEL_TILE_SELECT_STMT_IDX];
    stmt2 = conn->prepared_statements[MBTILES_DEL_TILE_STMT1_IDX];
    stmt3 = conn->prepared_statements[MBTILES_DEL_TILE_STMT2_IDX];
  }

  /* first extract tile_id from the tile we will delete. We need this because we do not know
   * if the tile is empty or not.
   * If it is empty, we will not delete the image blob data from the images table */
  cache->bind_stmt(ctx, stmt1, cache, tile);
  do {
    ret = sqlite3_step(stmt1);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite backend failed on mbtile del 1: %s", sqlite3_errmsg(conn->handle));
      sqlite3_reset(stmt1);
      mapcache_sqlite_release_conn(ctx, pc);
      return;
    }
  } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  if (ret == SQLITE_DONE) { /* tile does not exist, ignore */
    sqlite3_reset(stmt1);
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  } else {
    tile_id = (const char*) sqlite3_column_text(stmt1, 0);
    tile_id_size = sqlite3_column_bytes(stmt1, 0);
  }


  /* delete the tile from the "map" table */
  cache->bind_stmt(ctx,stmt2, cache, tile);
  ret = sqlite3_step(stmt2);
  if (ret != SQLITE_DONE && ret != SQLITE_ROW) {
    ctx->set_error(ctx, 500, "sqlite backend failed on mbtile del 2: %s", sqlite3_errmsg(conn->handle));
    sqlite3_reset(stmt1);
    sqlite3_reset(stmt2);
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }

  if(tile_id[0] != '#') {
    /* the tile isn't empty, we must also delete from the images table */
    int paramidx = sqlite3_bind_parameter_index(stmt3, ":foobar");
    if (paramidx) {
      sqlite3_bind_text(stmt3, paramidx, tile_id, tile_id_size, SQLITE_STATIC);
    }
    ret = sqlite3_step(stmt3);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx, 500, "sqlite backend failed on mbtile del 3: %s", sqlite3_errmsg(conn->handle));
      sqlite3_reset(stmt1);
      sqlite3_reset(stmt2);
      sqlite3_reset(stmt3);
      mapcache_sqlite_release_conn(ctx, pc);
      return;
    }
  }

  sqlite3_reset(stmt1);
  sqlite3_reset(stmt2);
  sqlite3_reset(stmt3);
  mapcache_sqlite_release_conn(ctx, pc);
}



static void _single_mbtile_set(mapcache_context *ctx, mapcache_cache_sqlite *cache, mapcache_tile *tile, struct sqlite_conn *conn)
{
  sqlite3_stmt *stmt1,*stmt2;
  int ret;
  if(!tile->raw_image) {
    tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
    GC_CHECK_ERROR(ctx);
  }
  if(mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
    stmt1 = conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT1_IDX];
    stmt2 = conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT2_IDX];
    if(!stmt1) {
      sqlite3_prepare(conn->handle,
                      "insert or ignore into images(tile_id,tile_data) values (:color,:data);",
                      -1, &conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT1_IDX], NULL);
      sqlite3_prepare(conn->handle,
                      "insert or replace into map(tile_column,tile_row,zoom_level,tile_id) values (:x,:y,:z,:color);",
                      -1, &conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT2_IDX], NULL);
      stmt1 = conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT1_IDX];
      stmt2 = conn->prepared_statements[MBTILES_SET_EMPTY_TILE_STMT2_IDX];
    }
    cache->bind_stmt(ctx, stmt1, cache, tile);
    cache->bind_stmt(ctx, stmt2, cache, tile);
  } else {
    stmt1 = conn->prepared_statements[MBTILES_SET_TILE_STMT1_IDX];
    stmt2 = conn->prepared_statements[MBTILES_SET_TILE_STMT2_IDX];
    if(!stmt1) {
      sqlite3_prepare(conn->handle,
                      "insert or replace into images(tile_id,tile_data) values (:key,:data);",
                      -1, &conn->prepared_statements[MBTILES_SET_TILE_STMT1_IDX], NULL);
      sqlite3_prepare(conn->handle,
                      "insert or replace into map(tile_column,tile_row,zoom_level,tile_id) values (:x,:y,:z,:key);",
                      -1, &conn->prepared_statements[MBTILES_SET_TILE_STMT2_IDX], NULL);
      stmt1 = conn->prepared_statements[MBTILES_SET_TILE_STMT1_IDX];
      stmt2 = conn->prepared_statements[MBTILES_SET_TILE_STMT2_IDX];
    }
    cache->bind_stmt(ctx, stmt1, cache, tile);
    cache->bind_stmt(ctx, stmt2, cache, tile);
  }
  do {
    ret = sqlite3_step(stmt1);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "mbtiles backend failed on image set: %s (%d)", sqlite3_errmsg(conn->handle), ret);
      break;
    }
    if (ret == SQLITE_BUSY) {
      sqlite3_reset(stmt1);
    }
  } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  if(ret == SQLITE_DONE) {
    do {
      ret = sqlite3_step(stmt2);
      if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
        ctx->set_error(ctx, 500, "mbtiles backend failed on map set: %s (%d)", sqlite3_errmsg(conn->handle), ret);
        break;
      }
      if (ret == SQLITE_BUSY) {
        sqlite3_reset(stmt2);
      }
    } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  }
  sqlite3_reset(stmt1);
  sqlite3_reset(stmt2);
}

static int _mapcache_cache_sqlite_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) pcache;
  struct sqlite_conn *conn;
  sqlite3_stmt *stmt;
  int ret;
  mapcache_pooled_connection *pc = mapcache_sqlite_get_conn(ctx,cache,tile,1);
  if (GC_HAS_ERROR(ctx)) {
    if(tile->tileset->read_only || !tile->tileset->source) {
      mapcache_sqlite_release_conn(ctx, pc);
      return MAPCACHE_FAILURE;
    } else {
      /* not an error in this case, as the db file may not have been created yet */
      ctx->clear_errors(ctx);
      mapcache_sqlite_release_conn(ctx, pc);
      return MAPCACHE_CACHE_MISS;
    }
  }
  conn = SQLITE_CONN(pc);
  stmt = conn->prepared_statements[GET_TILE_STMT_IDX];
  if(!stmt) {
    sqlite3_prepare(conn->handle, cache->get_stmt.sql, -1, &conn->prepared_statements[GET_TILE_STMT_IDX], NULL);
    stmt = conn->prepared_statements[GET_TILE_STMT_IDX];
  }
  cache->bind_stmt(ctx, stmt, cache, tile);
  do {
    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite backend failed on get: %s", sqlite3_errmsg(conn->handle));
      sqlite3_reset(stmt);
      mapcache_sqlite_release_conn(ctx, pc);
      return MAPCACHE_FAILURE;
    }
  } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  if (ret == SQLITE_DONE) {
    sqlite3_reset(stmt);
    mapcache_sqlite_release_conn(ctx, pc);
    return MAPCACHE_CACHE_MISS;
  } else {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int size = sqlite3_column_bytes(stmt, 0);
    if(size>0 && ((char*)blob)[0] == '#') {
      tile->encoded_data = mapcache_empty_png_decode(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy ,blob,&tile->nodata);
    } else {
      tile->encoded_data = mapcache_buffer_create(size, ctx->pool);
      memcpy(tile->encoded_data->buf, blob, size);
      tile->encoded_data->size = size;
    }
    if (sqlite3_column_count(stmt) > 1) {
      time_t mtime = sqlite3_column_int64(stmt, 1);
      apr_time_ansi_put(&(tile->mtime), mtime);
    }
    sqlite3_reset(stmt);
    mapcache_sqlite_release_conn(ctx, pc);
    return MAPCACHE_SUCCESS;
  }
}

static void _single_sqlitetile_set(mapcache_context *ctx, mapcache_cache_sqlite *cache, mapcache_tile *tile, struct sqlite_conn *conn)
{
  sqlite3_stmt *stmt = conn->prepared_statements[SQLITE_SET_TILE_STMT_IDX];
  int ret;

  if(!stmt) {
    sqlite3_prepare(conn->handle, cache->set_stmt.sql, -1, &conn->prepared_statements[SQLITE_SET_TILE_STMT_IDX], NULL);
    stmt = conn->prepared_statements[SQLITE_SET_TILE_STMT_IDX];
  }
  cache->bind_stmt(ctx, stmt, cache, tile);
  do {
    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
      ctx->set_error(ctx, 500, "sqlite backend failed on set: %s (%d)", sqlite3_errmsg(conn->handle), ret);
      break;
    }
    if (ret == SQLITE_BUSY) {
      sqlite3_reset(stmt);
    }
  } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
  sqlite3_reset(stmt);
}

static void _mapcache_cache_sqlite_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)pcache;
  struct sqlite_conn *conn;
  mapcache_pooled_connection *pc = mapcache_sqlite_get_conn(ctx,cache,tile,0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);
  sqlite3_exec(conn->handle, "BEGIN TRANSACTION", 0, 0, 0);
  _single_sqlitetile_set(ctx,cache, tile,conn);
  if (GC_HAS_ERROR(ctx)) {
    sqlite3_exec(conn->handle, "ROLLBACK TRANSACTION", 0, 0, 0);
  } else {
    sqlite3_exec(conn->handle, "END TRANSACTION", 0, 0, 0);
  }
  mapcache_sqlite_release_conn(ctx, pc);
}

static void _mapcache_cache_sqlite_multi_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)pcache;
  int i;
  struct sqlite_conn *conn;
  mapcache_pooled_connection *pc = mapcache_sqlite_get_conn(ctx,cache,&tiles[0],0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);
  sqlite3_exec(conn->handle, "BEGIN TRANSACTION", 0, 0, 0);
  for (i = 0; i < ntiles; i++) {
    mapcache_tile *tile = &tiles[i];
    _single_sqlitetile_set(ctx,cache, tile,conn);
    if(GC_HAS_ERROR(ctx)) break;
  }
  if (GC_HAS_ERROR(ctx)) {
    sqlite3_exec(conn->handle, "ROLLBACK TRANSACTION", 0, 0, 0);
  } else {
    sqlite3_exec(conn->handle, "END TRANSACTION", 0, 0, 0);
  }
  mapcache_sqlite_release_conn(ctx, pc);
}

static void _mapcache_cache_mbtiles_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)pcache;
  struct sqlite_conn *conn;
  mapcache_pooled_connection *pc = mapcache_sqlite_get_conn(ctx,cache,tile,0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);
  if(!tile->raw_image) {
    tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
    if(GC_HAS_ERROR(ctx)) {
      mapcache_sqlite_release_conn(ctx, pc);
      return;
    }
  }
  sqlite3_exec(conn->handle, "BEGIN TRANSACTION", 0, 0, 0);
  _single_mbtile_set(ctx, cache, tile,conn);
  if (GC_HAS_ERROR(ctx)) {
    sqlite3_exec(conn->handle, "ROLLBACK TRANSACTION", 0, 0, 0);
  } else {
    sqlite3_exec(conn->handle, "END TRANSACTION", 0, 0, 0);
  }
  mapcache_sqlite_release_conn(ctx, pc);
}

static void _mapcache_cache_mbtiles_multi_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  int i;
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)pcache;
  mapcache_pooled_connection *pc;
  struct sqlite_conn *conn;

  /* decode/encode image data before going into the sqlite write lock */
  for (i = 0; i < ntiles; i++) {
    mapcache_tile *tile = &tiles[i];
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    /* only encode to image format if tile is not blank */
    if (mapcache_image_blank_color(tile->raw_image) != MAPCACHE_TRUE && !tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
    }
  }
  pc = mapcache_sqlite_get_conn(ctx,cache,&tiles[0],0);
  if (GC_HAS_ERROR(ctx)) {
    mapcache_sqlite_release_conn(ctx, pc);
    return;
  }
  conn = SQLITE_CONN(pc);

  sqlite3_exec(conn->handle, "BEGIN TRANSACTION", 0, 0, 0);
  for (i = 0; i < ntiles; i++) {
    mapcache_tile *tile = &tiles[i];
    _single_mbtile_set(ctx,cache,tile,conn);
    if(GC_HAS_ERROR(ctx)) break;
  }
  if (GC_HAS_ERROR(ctx)) {
    sqlite3_exec(conn->handle, "ROLLBACK TRANSACTION", 0, 0, 0);
  } else {
    sqlite3_exec(conn->handle, "END TRANSACTION", 0, 0, 0);
  }
  mapcache_sqlite_release_conn(ctx, pc);
}

static void _mapcache_cache_sqlite_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *pcache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_sqlite *cache;
  sqlite3_initialize();
  sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  cache = (mapcache_cache_sqlite*) pcache;
  if ((cur_node = ezxml_child(node, "base")) != NULL) {
    ctx->set_error(ctx, 500, "sqlite config <base> not supported anymore, use <dbfile>");
    return;
  }
  if ((cur_node = ezxml_child(node, "dbname_template")) != NULL) {
    ctx->set_error(ctx, 500, "sqlite config <dbname_template> not supported anymore, use a \"multi-sqlite3\" cache type");
    return;
  }
  if ((cur_node = ezxml_child(node, "dbfile")) != NULL) {
    char *fmt;
    cache->dbfile = apr_pstrdup(ctx->pool, cur_node->txt);
    fmt = (char*)ezxml_attr(cur_node,"x_fmt");
    if(fmt && *fmt) {
      cache->x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"y_fmt");
    if(fmt && *fmt) {
      cache->y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"z_fmt");
    if(fmt && *fmt) {
      cache->z_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_x_fmt");
    if(fmt && *fmt) {
      cache->inv_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_y_fmt");
    if(fmt && *fmt) {
      cache->inv_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"div_x_fmt");
    if(fmt && *fmt) {
      cache->div_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"div_y_fmt");
    if(fmt && *fmt) {
      cache->div_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_div_x_fmt");
    if(fmt && *fmt) {
      cache->inv_div_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_div_y_fmt");
    if(fmt && *fmt) {
      cache->inv_div_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
  }
  
  cache->detect_blank = 0;
  if ((cur_node = ezxml_child(node, "detect_blank")) != NULL) {
    if(strcasecmp(cur_node->txt,"false")) {
      cache->detect_blank = 1;
    }
  }

  if ((cur_node = ezxml_child(node, "hitstats")) != NULL) {
    if (!strcasecmp(cur_node->txt, "true")) {
      ctx->set_error(ctx, 500, "sqlite config <hitstats> not supported anymore");
    }
  }
  if ((cur_node = ezxml_child(node, "pragma")) != NULL) {
    cache->pragmas = apr_table_make(ctx->pool,1);
    while(cur_node) {
      char *name = (char*)ezxml_attr(cur_node,"name");
      if(!name || !cur_node->txt || !strlen(cur_node->txt)) {
        ctx->set_error(ctx,500,"<pragma> missing name attribute");
        return;
      }
      apr_table_set(cache->pragmas,name,cur_node->txt);
      cur_node = cur_node->next;
    }
  }
  if ((cur_node = ezxml_child(node, "queries")) != NULL) {
    ezxml_t query_node;
    if ((query_node = ezxml_child(cur_node, "exists")) != NULL) {
      cache->exists_stmt.sql = apr_pstrdup(ctx->pool,query_node->txt);
    }
    if ((query_node = ezxml_child(cur_node, "get")) != NULL) {
      cache->get_stmt.sql = apr_pstrdup(ctx->pool,query_node->txt);
    }
    if ((query_node = ezxml_child(cur_node, "set")) != NULL) {
      cache->set_stmt.sql = apr_pstrdup(ctx->pool,query_node->txt);
    }
    if ((query_node = ezxml_child(cur_node, "delete")) != NULL) {
      cache->delete_stmt.sql = apr_pstrdup(ctx->pool,query_node->txt);
    }
    if ((query_node = ezxml_child(cur_node, "create")) != NULL) {
      cache->create_stmt.sql = apr_pstrdup(ctx->pool,query_node->txt);
    }
  }
  
  cur_node = ezxml_child(node,"xcount");
  if(cur_node && cur_node->txt && *cur_node->txt) {
    char *endptr;
    cache->count_x = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400,"failed to parse xcount value %s for sqlite cache %s", cur_node->txt,cache->cache.name);
      return;
    }
  }
  cur_node = ezxml_child(node,"ycount");
  if(cur_node && cur_node->txt && *cur_node->txt) {
    char *endptr;
    cache->count_y = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400,"failed to parse ycount value %s for sqlite cache %s", cur_node->txt,cache->cache.name);
      return;
    }
  }
}

/**
 * \private \memberof mapcache_cache_sqlite
 */
static void _mapcache_cache_sqlite_configuration_post_config(mapcache_context *ctx,
    mapcache_cache *pcache, mapcache_cfg *cfg)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)pcache;
  if (!cache->dbfile) {
    ctx->set_error(ctx, 500, "sqlite cache \"%s\" is missing <dbfile> entry", pcache->name);
    return;
  }
}

/**
 * \private \memberof mapcache_cache_sqlite
 */
static void _mapcache_cache_mbtiles_configuration_post_config(mapcache_context *ctx,
    mapcache_cache *pcache, mapcache_cfg *cfg)
{
  /* check that only one tileset/grid references this cache, as mbtiles does
   not support multiple tilesets/grids per cache */
#ifdef FIXME
  int nrefs = 0;
  apr_hash_index_t *tileseti = apr_hash_first(ctx->pool,cfg->tilesets);
  while(tileseti) {
    mapcache_tileset *tileset;
    const void *key;
    apr_ssize_t keylen;
    apr_hash_this(tileseti,&key,&keylen,(void**)&tileset);
    if(tileset->cache == cache) {
      nrefs++;
      if(nrefs>1) {
        ctx->set_error(ctx,500,"mbtiles cache %s is referenced by more than 1 tileset, which is not supported",cache->name);
        return;
      }
      if(tileset->grid_links->nelts > 1) {
        ctx->set_error(ctx,500,"mbtiles cache %s is referenced by tileset %s which has more than 1 grid, which is not supported",cache->name,tileset->name);
        return;
      }
    }
    tileseti = apr_hash_next(tileseti);
  }
#endif
}

mapcache_cache* mapcache_cache_sqlite_create(mapcache_context *ctx)
{
  mapcache_cache_sqlite *cache = apr_pcalloc(ctx->pool, sizeof (mapcache_cache_sqlite));
  if (!cache) {
    ctx->set_error(ctx, 500, "failed to allocate sqlite cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool, 3);
  cache->cache.type = MAPCACHE_CACHE_SQLITE;
  cache->cache._tile_delete = _mapcache_cache_sqlite_delete;
  cache->cache._tile_get = _mapcache_cache_sqlite_get;
  cache->cache._tile_exists = _mapcache_cache_sqlite_has_tile;
  cache->cache._tile_set = _mapcache_cache_sqlite_set;
  cache->cache._tile_multi_set = _mapcache_cache_sqlite_multi_set;
  cache->cache.configuration_post_config = _mapcache_cache_sqlite_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_sqlite_configuration_parse_xml;
  cache->create_stmt.sql = apr_pstrdup(ctx->pool,
                                       "create table if not exists tiles(tileset text, grid text, x integer, y integer, z integer, data blob, dim text, ctime datetime, primary key(tileset,grid,x,y,z,dim))");
  cache->exists_stmt.sql = apr_pstrdup(ctx->pool,
                                       "select 1 from tiles where x=:x and y=:y and z=:z and dim=:dim and tileset=:tileset and grid=:grid");
  cache->get_stmt.sql = apr_pstrdup(ctx->pool,
                                    "select data,strftime(\"%s\",ctime) from tiles where tileset=:tileset and grid=:grid and x=:x and y=:y and z=:z and dim=:dim");
  cache->set_stmt.sql = apr_pstrdup(ctx->pool,
                                    "insert or replace into tiles(tileset,grid,x,y,z,data,dim,ctime) values (:tileset,:grid,:x,:y,:z,:data,:dim,datetime('now'))");
  cache->delete_stmt.sql = apr_pstrdup(ctx->pool,
                                       "delete from tiles where x=:x and y=:y and z=:z and dim=:dim and tileset=:tileset and grid=:grid");
  cache->n_prepared_statements = 4;
  cache->bind_stmt = _bind_sqlite_params;
  cache->detect_blank = 1;
  cache->x_fmt = cache->y_fmt = cache->z_fmt
          = cache->inv_x_fmt = cache->inv_y_fmt
          = cache->div_x_fmt = cache->div_y_fmt
          = cache->inv_div_x_fmt = cache->inv_div_y_fmt = apr_pstrdup(ctx->pool,"%d");
  cache->count_x = cache->count_y = -1;
  return (mapcache_cache*)cache;
}


/**
 * \brief creates and initializes a mapcache_sqlite_cache
 */
mapcache_cache* mapcache_cache_mbtiles_create(mapcache_context *ctx)
{
  mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) mapcache_cache_sqlite_create(ctx);
  if (!cache) {
    return NULL;
  }
  cache->cache.configuration_post_config = _mapcache_cache_mbtiles_configuration_post_config;
  cache->cache._tile_set = _mapcache_cache_mbtiles_set;
  cache->cache._tile_multi_set = _mapcache_cache_mbtiles_multi_set;
  cache->cache._tile_delete = _mapcache_cache_mbtiles_delete;
  cache->create_stmt.sql = apr_pstrdup(ctx->pool,
                                       "create table if not exists images(tile_id text, tile_data blob, primary key(tile_id));"\
                                       "CREATE TABLE  IF NOT EXISTS map (zoom_level integer, tile_column integer, tile_row integer, tile_id text, foreign key(tile_id) references images(tile_id), primary key(tile_row,tile_column,zoom_level));"\
                                       "create table if not exists metadata(name text, value text);"\
                                       "create view if not exists tiles AS SELECT map.zoom_level AS zoom_level, map.tile_column AS tile_column, map.tile_row AS tile_row, images.tile_data AS tile_data FROM map JOIN images ON images.tile_id = map.tile_id;"
                                      );
  cache->exists_stmt.sql = apr_pstrdup(ctx->pool,
                                       "select 1 from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
  cache->get_stmt.sql = apr_pstrdup(ctx->pool,
                                    "select tile_data from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
  cache->delete_stmt.sql = apr_pstrdup(ctx->pool,
                                       "delete from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
  cache->n_prepared_statements = 9;
  cache->bind_stmt = _bind_mbtiles_params;
  return (mapcache_cache*) cache;
}
#else

mapcache_cache* mapcache_cache_sqlite_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"SQLITE support not compiled in this version");
  return NULL;
}
mapcache_cache* mapcache_cache_mbtiles_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"SQLITE (MBtiles) support not compiled in this version");
  return NULL;
}


#endif

/* vim: ts=2 sts=2 et sw=2
 */
