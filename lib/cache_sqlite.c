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

#ifdef USE_SQLITE

#include "mapcache.h"
#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <sqlite3.h>

struct sqlite_conn {
    sqlite3 *handle;
    int readonly;
};

static struct sqlite_conn* _sqlite_get_conn(mapcache_context *ctx, mapcache_tile* tile, int readonly) {
   apr_status_t rv;
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   struct sqlite_conn *conn;
   apr_reslist_t *pool;
   if(readonly) {
       pool = cache->ro_connection_pool;
   } else {
       pool = cache->rw_connection_pool;
   }
   rv = apr_reslist_acquire(pool, (void **)&conn);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx,500,"failed to aquire connection to sqlite backend: %s", cache->ctx->get_error_message(cache->ctx));
      cache->ctx->clear_errors(cache->ctx);
      return NULL;
   }
   return conn;
}

static void _sqlite_release_conn(mapcache_context *ctx, mapcache_tile *tile, struct sqlite_conn *conn) {
   mapcache_cache_sqlite* cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   apr_reslist_t *pool;
   if(conn->readonly) 
       pool = cache->ro_connection_pool;
   else
       pool = cache->rw_connection_pool;
   
   if(GC_HAS_ERROR(ctx)) {
      apr_reslist_invalidate(pool,(void*)conn);  
   } else {
      apr_reslist_release(pool, (void*)conn);
   }
}
static apr_status_t _sqlite_reslist_get_rw_connection(void **conn_, void *params, apr_pool_t *pool) {
   int ret;
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)params;
   struct sqlite_conn *conn = apr_pcalloc(pool,sizeof(struct sqlite_conn));
   int flags;
   flags = SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_CREATE;
   ret = sqlite3_open_v2(cache->dbfile,&conn->handle,flags,NULL);
   if(ret != SQLITE_OK) {
      cache->ctx->set_error(cache->ctx, 500, "sqlite backend failed to open db %s: %s", cache->dbfile, sqlite3_errmsg(conn->handle));
      return APR_EGENERAL;
   }
   sqlite3_busy_timeout(conn->handle,300000);
   do {
      ret = sqlite3_exec(conn->handle, cache->create_stmt.sql, 0, 0, NULL);
      if(ret != SQLITE_OK && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
         break;
      }
   } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
   if(ret != SQLITE_OK) {
      cache->ctx->set_error(cache->ctx, 500, "sqlite backend failed to create db schema on %s: %s",cache->dbfile, sqlite3_errmsg(conn->handle));
      sqlite3_close(conn->handle);
      return APR_EGENERAL;
   }
   conn->readonly = 0;
   *conn_ = conn;
   return APR_SUCCESS;
}

static apr_status_t _sqlite_reslist_get_ro_connection(void **conn_, void *params, apr_pool_t *pool) {
   int ret;
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)params;
   struct sqlite_conn *conn = apr_pcalloc(pool,sizeof(struct sqlite_conn));
   int flags;
   flags = SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX;
   ret = sqlite3_open_v2(cache->dbfile,&conn->handle,flags,NULL);
   if(ret != SQLITE_OK) {
      /* maybe the database file doesn't exist yet. so we create it and setup the schema */
      ret = sqlite3_open_v2(cache->dbfile, &conn->handle,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL);
      if (ret != SQLITE_OK) {
         cache->ctx->set_error(cache->ctx, 500, "sqlite backend failed to open db %s: %s", cache->dbfile, sqlite3_errmsg(conn->handle));
         sqlite3_close(conn->handle);
         return APR_EGENERAL;
      }
      sqlite3_busy_timeout(conn->handle,300000);
      do {
         ret = sqlite3_exec(conn->handle, cache->create_stmt.sql, 0, 0, NULL);
         if(ret != SQLITE_OK && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
            break;
         }
      } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
      if(ret != SQLITE_OK) {
         cache->ctx->set_error(cache->ctx, 500, "sqlite backend failed to create db schema on %s: %s",cache->dbfile, sqlite3_errmsg(conn->handle));
         sqlite3_close(conn->handle);
         return APR_EGENERAL;
      }

      sqlite3_close(conn->handle);
      ret = sqlite3_open_v2(cache->dbfile,&conn->handle,flags,NULL);
      if (ret != SQLITE_OK) {
         cache->ctx->set_error(cache->ctx, 500, "sqlite backend failed to re-open freshly created db %s readonly: %s",cache->dbfile, sqlite3_errmsg(conn->handle));
         sqlite3_close(conn->handle);
         return APR_EGENERAL;
      }
   }
   sqlite3_busy_timeout(conn->handle,300000);
   conn->readonly = 1;

   *conn_ = conn;
   return APR_SUCCESS;
}

static apr_status_t _sqlite_reslist_free_connection(void *conn_, void *params, apr_pool_t *pool) {
   struct sqlite_conn *conn = (struct sqlite_conn*)conn_;
   sqlite3_close(conn->handle);
   return APR_SUCCESS; 
}

/**
 * \brief apply appropriate tile properties to the sqlite statement */
static void _bind_sqlite_params(mapcache_context *ctx, sqlite3_stmt *stmt, mapcache_tile *tile) {
   int paramidx;
   /* tile->x */
   paramidx = sqlite3_bind_parameter_index(stmt, ":x");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->x);
   
   /* tile->y */
   paramidx = sqlite3_bind_parameter_index(stmt, ":y");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->y);
   
   /* tile->y */
   paramidx = sqlite3_bind_parameter_index(stmt, ":z");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->z);
  
   /* eventual dimensions */
   paramidx = sqlite3_bind_parameter_index(stmt, ":dim");
   if(paramidx) {
      if(tile->dimensions) {
         char *dim = mapcache_util_get_tile_dimkey(ctx,tile,NULL,NULL);
         sqlite3_bind_text(stmt,paramidx,dim,-1,SQLITE_STATIC);
      } else {
         sqlite3_bind_text(stmt,paramidx,"",-1,SQLITE_STATIC);
      }
   }
   
   /* grid */
   paramidx = sqlite3_bind_parameter_index(stmt, ":grid");
   if(paramidx) sqlite3_bind_text(stmt,paramidx,tile->grid_link->grid->name,-1,SQLITE_STATIC);
   
   /* tileset */
   paramidx = sqlite3_bind_parameter_index(stmt, ":tileset");
   if(paramidx) sqlite3_bind_text(stmt,paramidx,tile->tileset->name,-1,SQLITE_STATIC);
   
   /* tile blob data */
   paramidx = sqlite3_bind_parameter_index(stmt, ":data");
   if(paramidx) {
      if(!tile->encoded_data) {
         tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
         GC_CHECK_ERROR(ctx);
      }
      if(tile->encoded_data && tile->encoded_data->size) {
         sqlite3_bind_blob(stmt,paramidx,tile->encoded_data->buf, tile->encoded_data->size,SQLITE_STATIC);
      } else {
         sqlite3_bind_text(stmt,paramidx,"",-1,SQLITE_STATIC);
      }
   }
}

static int _mapcache_cache_sqlite_has_tile(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   struct sqlite_conn *conn = _sqlite_get_conn(ctx,tile,1);
   sqlite3_stmt *stmt;
   int ret;
   if(GC_HAS_ERROR(ctx)) {
      _sqlite_release_conn(ctx,tile,conn);
      return MAPCACHE_FALSE;
   }

   sqlite3_prepare(conn->handle,cache->exists_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on has_tile: %s",sqlite3_errmsg(conn->handle));
   }
   if(ret == SQLITE_DONE) {
      ret = MAPCACHE_FALSE;
   } else if(ret == SQLITE_ROW){
      ret = MAPCACHE_TRUE;
   }
   sqlite3_finalize(stmt);
   _sqlite_release_conn(ctx,tile,conn);
   return ret;
}

static void _mapcache_cache_sqlite_delete(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   struct sqlite_conn *conn = _sqlite_get_conn(ctx,tile,0);
   sqlite3_stmt *stmt;
   int ret;
   if(GC_HAS_ERROR(ctx)) {
       _sqlite_release_conn(ctx,tile,conn);
       return;
   }
   sqlite3_prepare(conn->handle,cache->delete_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on delete: %s",sqlite3_errmsg(conn->handle));
   }
   sqlite3_finalize(stmt);
   _sqlite_release_conn(ctx,tile,conn);
}


static int _mapcache_cache_sqlite_get(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   struct sqlite_conn *conn;
   sqlite3_stmt *stmt;
   int ret;
   conn = _sqlite_get_conn(ctx,tile,1);
   if(GC_HAS_ERROR(ctx)) {
      _sqlite_release_conn(ctx,tile,conn);
      return MAPCACHE_FAILURE;
   }
   sqlite3_prepare(conn->handle,cache->get_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   do {
      ret = sqlite3_step(stmt);
      if(ret!=SQLITE_DONE && ret != SQLITE_ROW && ret!=SQLITE_BUSY && ret !=SQLITE_LOCKED) {
         ctx->set_error(ctx,500,"sqlite backend failed on get: %s",sqlite3_errmsg(conn->handle));
         sqlite3_finalize(stmt);
         _sqlite_release_conn(ctx,tile,conn);
         return MAPCACHE_FAILURE;
      }
   } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
   if(ret == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      _sqlite_release_conn(ctx,tile,conn);
      return MAPCACHE_CACHE_MISS;
   } else {
      const void *blob = sqlite3_column_blob(stmt,0);
      int size = sqlite3_column_bytes(stmt, 0);
      tile->encoded_data = mapcache_buffer_create(size,ctx->pool);
      memcpy(tile->encoded_data->buf, blob,size);
      tile->encoded_data->size = size;
      if(sqlite3_column_count(stmt) > 1) {
         time_t mtime = sqlite3_column_int64(stmt, 1);
         apr_time_ansi_put(&(tile->mtime),mtime);
      }
      sqlite3_finalize(stmt);
      _sqlite_release_conn(ctx,tile,conn);
      return MAPCACHE_SUCCESS;
   }
}

static void _mapcache_cache_sqlite_set(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   struct sqlite_conn *conn = _sqlite_get_conn(ctx,tile,0);
   sqlite3_stmt *stmt;
   int ret;
   GC_CHECK_ERROR(ctx);
   
   sqlite3_prepare(conn->handle,cache->set_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   do {
      ret = sqlite3_step(stmt);
      if(ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
         ctx->set_error(ctx,500,"sqlite backend failed on set: %s (%d)",sqlite3_errmsg(conn->handle),ret);
         break;
      }
      if(ret == SQLITE_BUSY) {
         sqlite3_reset(stmt);
      }
   } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
   sqlite3_finalize(stmt);
   _sqlite_release_conn(ctx,tile,conn);
}

static void _mapcache_cache_sqlite_multi_set(mapcache_context *ctx, mapcache_tile *tiles, int ntiles) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tiles[0].tileset->cache;
   struct sqlite_conn *conn = _sqlite_get_conn(ctx,&tiles[0],0);
   sqlite3_stmt *stmt;
   int ret,i;
   GC_CHECK_ERROR(ctx);
   sqlite3_prepare(conn->handle,cache->set_stmt.sql,-1,&stmt,NULL);
   sqlite3_exec(conn->handle, "BEGIN TRANSACTION", 0, 0, 0);
   for(i=0;i<ntiles;i++) {
      mapcache_tile *tile = &tiles[i];
      _bind_sqlite_params(ctx,stmt,tile);
      do {
         ret = sqlite3_step(stmt);
         if(ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
            ctx->set_error(ctx,500,"sqlite backend failed on set: %s (%d)",sqlite3_errmsg(conn->handle),ret);
            break;
         }
         if(ret == SQLITE_BUSY) {
            sqlite3_reset(stmt);
         }
      } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
      if(GC_HAS_ERROR(ctx)) break;
      sqlite3_clear_bindings(stmt);
      sqlite3_reset(stmt);
   }
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_exec(conn->handle, "ROLLBACK TRANSACTION", 0, 0, 0);
   } else {
      sqlite3_exec(conn->handle, "END TRANSACTION", 0, 0, 0);
   }
   sqlite3_finalize(stmt);
   _sqlite_release_conn(ctx,&tiles[0],conn);
}

static void _mapcache_cache_sqlite_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
   ezxml_t cur_node;
   mapcache_cache_sqlite *dcache;
   sqlite3_initialize();
   apr_status_t rv;
   sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
   dcache = (mapcache_cache_sqlite*)cache;
   if ((cur_node = ezxml_child(node,"base")) != NULL) {
      ctx->set_error(ctx,500,"sqlite config <base> not supported anymore, use <dbfile>");
      return;
   }
   if ((cur_node = ezxml_child(node,"dbname_template")) != NULL) {
      ctx->set_error(ctx,500,"sqlite config <dbname_template> not supported anymore, use <dbfile>");
      return;
   }
   if ((cur_node = ezxml_child(node,"dbfile")) != NULL) {
      dcache->dbfile = apr_pstrdup(ctx->pool, cur_node->txt);
   }
   if ((cur_node = ezxml_child(node,"hitstats")) != NULL) {
      if(!strcasecmp(cur_node->txt,"true")) {
         ctx->set_error(ctx,500,"sqlite config <hitstats> not supported anymore");
      }
   }
   if(!dcache->dbfile) {
      ctx->set_error(ctx,500,"sqlite cache \"%s\" is missing <dbfile> entry",cache->name);
      return;
   }
   dcache->ctx = ctx;
   rv = apr_reslist_create(&(dcache->ro_connection_pool),
         0 /* min */,
         10 /* soft max */,
         200 /* hard max */,
         60*1000000 /*60 seconds, ttl*/,
         _sqlite_reslist_get_ro_connection, /* resource constructor */
         _sqlite_reslist_free_connection, /* resource destructor */
         dcache, ctx->pool);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx,500,"failed to create sqlite read-only connection pool");
      return;
   }
   apr_pool_cleanup_register(ctx->pool, dcache->ro_connection_pool,(void*)apr_reslist_destroy,apr_pool_cleanup_null) ;
   rv = apr_reslist_create(&(dcache->rw_connection_pool),
         0 /* min */,
         1 /* soft max */,
         1 /* hard max */,
         60*1000000 /*60 seconds, ttl*/,
         _sqlite_reslist_get_rw_connection, /* resource constructor */
         _sqlite_reslist_free_connection, /* resource destructor */
         dcache, ctx->pool);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx,500,"failed to create sqlite read-write connection pool");
      return;
   }
   apr_pool_cleanup_register(ctx->pool, dcache->rw_connection_pool,(void*)apr_reslist_destroy,apr_pool_cleanup_null) ;
}
   
/**
 * \private \memberof mapcache_cache_sqlite
 */
static void _mapcache_cache_sqlite_configuration_post_config(mapcache_context *ctx,
      mapcache_cache *cache, mapcache_cfg *cfg) {
}

/**
 * \brief creates and initializes a mapcache_sqlite_cache
 */
mapcache_cache* mapcache_cache_sqlite_create(mapcache_context *ctx) {
   mapcache_cache_sqlite *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_sqlite));
   if(!cache) {
      ctx->set_error(ctx, 500, "failed to allocate sqlite cache");
      return NULL;
   }
   cache->cache.metadata = apr_table_make(ctx->pool,3);
   cache->cache.type = MAPCACHE_CACHE_SQLITE;
   cache->cache.tile_delete = _mapcache_cache_sqlite_delete;
   cache->cache.tile_get = _mapcache_cache_sqlite_get;
   cache->cache.tile_exists = _mapcache_cache_sqlite_has_tile;
   cache->cache.tile_set = _mapcache_cache_sqlite_set;
   cache->cache.tile_multi_set = _mapcache_cache_sqlite_multi_set;
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
   return (mapcache_cache*)cache;
}

/**
 * \brief creates and initializes a mapcache_sqlite_cache
 */
mapcache_cache* mapcache_cache_mbtiles_create(mapcache_context *ctx) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)mapcache_cache_sqlite_create(ctx);
   if(!cache) {
      return NULL;
   }
   cache->create_stmt.sql = apr_pstrdup(ctx->pool,
         "CREATE TABLE  IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, primary key(tile_row, tile_column, zoom_level)); create table if not exists metadata(name text, value text);");
   cache->exists_stmt.sql = apr_pstrdup(ctx->pool,
         "select 1 from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   cache->get_stmt.sql = apr_pstrdup(ctx->pool,
         "select tile_data from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   cache->set_stmt.sql = apr_pstrdup(ctx->pool,
         "insert or replace into tiles(tile_column,tile_row,zoom_level,tile_data) values (:x,:y,:z,:data)");
   cache->delete_stmt.sql = apr_pstrdup(ctx->pool,
         "delete from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   return (mapcache_cache*)cache;
}

#endif

/* vim: ai ts=3 sts=3 et sw=3
*/
