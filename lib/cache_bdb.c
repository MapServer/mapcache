/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Berkeley DB cache backend
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

#include "mapcache-config.h"
#ifdef USE_BDB

#include "mapcache.h"
#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_file_info.h>
#include <apr_hash.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifdef APR_HAS_THREADS
#include <apr_thread_mutex.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#include <db.h>

#define PAGESIZE 64*1024
#define CACHESIZE 1024*1024

struct bdb_env {
  DB* db;
  DB_ENV *env;
  int readonly;
  char *errmsg;
};

void mapcache_bdb_connection_constructor(mapcache_context *ctx, void **conn_, void *params, apr_pool_t *pool)
{
  int ret;
  int env_flags;
  int mode;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)params;
  char *dbfile = apr_pstrcat(pool,cache->basedir,"/",cache->cache.name,".db",NULL);
  struct bdb_env *benv = calloc(1,sizeof(struct bdb_env));
  *conn_ = benv;

  ret = db_env_create(&benv->env, 0);
  if(ret) {
    ctx->set_error(ctx, 500, "bdb cache failure for db_env_create: %s", db_strerror(ret));
    free(benv);
    return;
  }
  ret = benv->env->set_cachesize(benv->env,0,CACHESIZE,1); /* set a larger cache size than default */
  if(ret) {
    ctx->set_error(ctx, 500, "bdb cache failure for db->set_cachesize: %s", db_strerror(ret));
    free(benv);
    return;
  }
  env_flags = DB_INIT_CDB|DB_INIT_MPOOL|DB_CREATE;
  ret = benv->env->open(benv->env,cache->basedir,env_flags,0);
  if(ret) {
    ctx->set_error(ctx,500,"bdb cache failure for env->open: %s", db_strerror(ret));
    free(benv);
    return;
  }

  if ((ret = db_create(&benv->db, benv->env, 0)) != 0) {
    ctx->set_error(ctx,500,"bdb cache failure for db_create: %s", db_strerror(ret));
    free(benv);
  }
  mode = DB_BTREE;
  ret = benv->db->set_pagesize(benv->db,PAGESIZE); /* set pagesize to maximum allowed, as tile data is usually pretty large */
  if(ret) {
    ctx->set_error(ctx,500,"bdb cache failure for db->set_pagesize: %s", db_strerror(ret));
    free(benv);
    return;
  }

  if ((ret = benv->db->open(benv->db, NULL, dbfile, NULL, mode, DB_CREATE, 0664)) != 0) {
    ctx->set_error(ctx,500,"bdb cache failure 1 for db->open: %s", db_strerror(ret));
    free(benv);
    return;
  }
}

void mapcache_bdb_connection_destructor(void *conn_, apr_pool_t *pool)
{
  struct bdb_env *benv = (struct bdb_env*)conn_;
  benv->db->close(benv->db,0);
  benv->env->close(benv->env,0);
  free(benv);
}



static mapcache_pooled_connection* _bdb_get_conn(mapcache_context *ctx, mapcache_cache_bdb *cache, mapcache_tile* tile, int readonly) {
  struct bdb_env *benv;
  mapcache_pooled_connection *pc;
  char *conn_key = apr_pstrcat(ctx->pool,readonly?"ro_":"rw_",cache->cache.name,NULL);
  pc = mapcache_connection_pool_get_connection(ctx,conn_key,mapcache_bdb_connection_constructor, mapcache_bdb_connection_destructor, cache);
  if(GC_HAS_ERROR(ctx)) return NULL;
  benv = pc->connection;
  benv->readonly = readonly;
  return pc;
}

static void _bdb_release_conn(mapcache_context *ctx, mapcache_cache_bdb *cache, mapcache_tile *tile, mapcache_pooled_connection *pc)
{
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx, pc);
  } else {
    mapcache_connection_pool_release_connection(ctx,pc);
  }
}

static int _mapcache_cache_bdb_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  int ret;
  DBT key;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)pcache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  mapcache_pooled_connection *pc;
  struct bdb_env *benv; 
  pc = _bdb_get_conn(ctx,cache,tile,1);
  if(GC_HAS_ERROR(ctx)) return MAPCACHE_FALSE;
  benv = pc->connection;
  memset(&key, 0, sizeof(DBT));
  key.data = skey;
  key.size = strlen(skey)+1;

  ret = benv->db->exists(benv->db, NULL, &key, 0);

  if(ret == 0) {
    ret = MAPCACHE_TRUE;
  } else if(ret == DB_NOTFOUND) {
    ret = MAPCACHE_FALSE;
  } else {
    ctx->set_error(ctx,500,"bdb backend failure on tile_exists: %s",db_strerror(ret));
    ret= MAPCACHE_FALSE;
  }
  _bdb_release_conn(ctx,cache,tile,pc);
  return ret;
}

static void _mapcache_cache_bdb_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  DBT key;
  int ret;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)pcache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  mapcache_pooled_connection *pc;
  struct bdb_env *benv; 
  pc = _bdb_get_conn(ctx,cache,tile,0);
  GC_CHECK_ERROR(ctx);
  benv = pc->connection;
  memset(&key, 0, sizeof(DBT));
  key.data = skey;
  key.size = strlen(skey)+1;
  ret = benv->db->del(benv->db, NULL, &key, 0);
  if(ret && ret != DB_NOTFOUND) {
    ctx->set_error(ctx,500,"bdb backend failure on tile_delete: %s",db_strerror(ret));
  } else {
    ret = benv->db->sync(benv->db,0);
    if(ret)
      ctx->set_error(ctx,500,"bdb backend sync failure on tile_delete: %s",db_strerror(ret));
  }
  _bdb_release_conn(ctx,cache,tile,pc);
}

static int _mapcache_cache_bdb_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  DBT key,data;
  int ret;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)pcache;
  mapcache_pooled_connection *pc;
  struct bdb_env *benv; 
  pc = _bdb_get_conn(ctx,cache,tile,1);
  if(GC_HAS_ERROR(ctx)) return MAPCACHE_FALSE;
  benv = pc->connection;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  memset(&key, 0, sizeof(DBT));
  memset(&data, 0, sizeof(DBT));
  data.flags = DB_DBT_MALLOC;
  key.data = skey;
  key.size = strlen(skey)+1;

  ret = benv->db->get(benv->db, NULL, &key, &data, 0);


  if(ret == 0) {
    if(((char*)(data.data))[0] == '#') {
      tile->encoded_data = mapcache_empty_png_decode(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy, (unsigned char*)data.data,&tile->nodata);
    } else {
      tile->encoded_data = mapcache_buffer_create(0,ctx->pool);
      tile->encoded_data->buf = data.data;
      tile->encoded_data->size = data.size-sizeof(apr_time_t);
      tile->encoded_data->avail = data.size;
      apr_pool_cleanup_register(ctx->pool, tile->encoded_data->buf,(void*)free, apr_pool_cleanup_null);
    }
    tile->mtime = *((apr_time_t*)(((char*)data.data)+data.size-sizeof(apr_time_t)));
    ret = MAPCACHE_SUCCESS;
  } else if(ret == DB_NOTFOUND) {
    ret = MAPCACHE_CACHE_MISS;
  } else {
    ctx->set_error(ctx,500,"bdb backend failure on tile_get: %s",db_strerror(ret));
    ret = MAPCACHE_FAILURE;
  }
  _bdb_release_conn(ctx,cache,tile,pc);
  return ret;
}


static void _mapcache_cache_bdb_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  DBT key,data;
  int ret;
  apr_time_t now;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)pcache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  mapcache_pooled_connection *pc;
  struct bdb_env *benv; 
  now = apr_time_now();
  memset(&key, 0, sizeof(DBT));
  memset(&data, 0, sizeof(DBT));

  key.data = skey;
  key.size = strlen(skey)+1;

  if(!tile->raw_image) {
    tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
    GC_CHECK_ERROR(ctx);
  }
  
  if(tile->raw_image->h==256 && tile->raw_image->w==256 && mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
    data.size = 5+sizeof(apr_time_t);
    data.data = apr_palloc(ctx->pool,data.size);
    (((char*)data.data)[0])='#';
    memcpy(((char*)data.data)+1,tile->raw_image->data,4);
    memcpy(((char*)data.data)+5,&now,sizeof(apr_time_t));
  } else {
    if(!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
    }
    mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
    data.data = tile->encoded_data->buf;
    data.size = tile->encoded_data->size;
    tile->encoded_data->size -= sizeof(apr_time_t);
  }
  
  pc = _bdb_get_conn(ctx,cache,tile,0);
  GC_CHECK_ERROR(ctx);
  benv = pc->connection;


  ret = benv->db->put(benv->db,NULL,&key,&data,0);
  if(ret != 0) {
    ctx->set_error(ctx,500,"dbd backend failed on tile_set: %s", db_strerror(ret));
  } else {
    ret = benv->db->sync(benv->db,0);
    if(ret)
      ctx->set_error(ctx,500,"bdb backend sync failure on tile_set: %s",db_strerror(ret));
  }
  _bdb_release_conn(ctx,cache,tile,pc);
}

static void _mapcache_cache_bdb_multiset(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  DBT key,data;
  int ret,i;
  apr_time_t now;
  mapcache_cache_bdb *cache = (mapcache_cache_bdb*)pcache;
  mapcache_pooled_connection *pc;
  struct bdb_env *benv; 
  now = apr_time_now();
  memset(&key, 0, sizeof(DBT));
  memset(&data, 0, sizeof(DBT));

  pc = _bdb_get_conn(ctx,cache,&tiles[0],0);
  GC_CHECK_ERROR(ctx);
  benv = pc->connection;
  
  for(i=0; i<ntiles; i++) {
    char *skey;
    mapcache_tile *tile;
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));
    tile = &tiles[i];
    skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      if(GC_HAS_ERROR(ctx)) {
        _bdb_release_conn(ctx,cache,&tiles[0],pc);
        return;
      }
    }
    if(tile->raw_image->h==256 && tile->raw_image->w==256 && mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      data.size = 5+sizeof(apr_time_t);
      data.data = apr_palloc(ctx->pool,data.size);
      (((char*)data.data)[0])='#';
      memcpy(((char*)data.data)+1,tile->raw_image->data,4);
      memcpy(((char*)data.data)+5,&now,sizeof(apr_time_t));
    } else {
      if(!tile->encoded_data) {
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        if(GC_HAS_ERROR(ctx)) {
          _bdb_release_conn(ctx,cache,&tiles[0],pc);
          return;
        }
      }
      mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
      data.data = tile->encoded_data->buf;
      data.size = tile->encoded_data->size;
      tile->encoded_data->size -= sizeof(apr_time_t);
    }
    key.data = skey;
    key.size = strlen(skey)+1;

    ret = benv->db->put(benv->db,NULL,&key,&data,0);
    if(ret != 0) {
      ctx->set_error(ctx,500,"dbd backend failed on tile_multiset: %s", db_strerror(ret));
      break;
    }
  }
  if(ret == 0) {
    ret = benv->db->sync(benv->db,0);
    if(ret)
      ctx->set_error(ctx,500,"bdb backend sync failure on sync in tile_multiset: %s",db_strerror(ret));
  }
  _bdb_release_conn(ctx,cache,&tiles[0],pc);
}


static void _mapcache_cache_bdb_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_bdb *dcache = (mapcache_cache_bdb*)cache;
  if ((cur_node = ezxml_child(node,"base")) != NULL) {
    dcache->basedir = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  if ((cur_node = ezxml_child(node,"key_template")) != NULL) {
    dcache->key_template = apr_pstrdup(ctx->pool,cur_node->txt);
  } else {
    dcache->key_template = apr_pstrdup(ctx->pool,"{tileset}-{grid}-{dim}-{z}-{y}-{x}.{ext}");
  }
  if(!dcache->basedir) {
    ctx->set_error(ctx,500,"dbd cache \"%s\" is missing <base> entry",cache->name);
    return;
  }
}

/**
 * \private \memberof mapcache_cache_dbd
 */
static void _mapcache_cache_bdb_configuration_post_config(mapcache_context *ctx,
    mapcache_cache *cache, mapcache_cfg *cfg)
{
  mapcache_cache_bdb *dcache = (mapcache_cache_bdb*)cache;
  apr_status_t rv;
  apr_dir_t *dir;
  rv = apr_dir_open(&dir, dcache->basedir, ctx->pool);
  if(rv != APR_SUCCESS) {
    char errmsg[120];
    ctx->set_error(ctx,500,"bdb failed to open directory %s:%s",dcache->basedir,apr_strerror(rv,errmsg,120));
  }
}

/**
 * \brief creates and initializes a mapcache_dbd_cache
 */
mapcache_cache* mapcache_cache_bdb_create(mapcache_context *ctx)
{
  mapcache_cache_bdb *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_bdb));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate berkeley db cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_BDB;
  cache->cache.tile_delete = _mapcache_cache_bdb_delete;
  cache->cache.tile_get = _mapcache_cache_bdb_get;
  cache->cache.tile_exists = _mapcache_cache_bdb_has_tile;
  cache->cache.tile_set = _mapcache_cache_bdb_set;
  cache->cache.tile_multi_set = _mapcache_cache_bdb_multiset;
  cache->cache.configuration_post_config = _mapcache_cache_bdb_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_bdb_configuration_parse_xml;
  cache->basedir = NULL;
  cache->key_template = NULL;
  return (mapcache_cache*)cache;
}

#endif

/* vim: ts=2 sts=2 et sw=2
*/
