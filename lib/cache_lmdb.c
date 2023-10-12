/******************************************************************************
 *
 * Project:  MapCache
 * Purpose:  MapCache tile caching support file: LMDB cache backend
 * Author:   Maris Nartiss and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2022 Regents of the University of Minnesota.
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
#ifdef USE_LMDB
#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_file_info.h>
#include <apr_hash.h>
#include <apr_cstr.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "lmdb.h"

typedef struct mapcache_cache_lmdb mapcache_cache_lmdb;
struct mapcache_cache_lmdb {
  mapcache_cache cache;
  char *basedir;
  char *key_template;
  size_t max_size;
  unsigned int max_readers;
  MDB_env *env;
};

/* LMDB env should be opened only once per process */
typedef struct lmdb_env_s lmdb_env_s;
struct lmdb_env_s {
  MDB_env *env;
  MDB_dbi dbi;
  int is_open;
};

static lmdb_env_s *lmdb_env;

static int _mapcache_cache_lmdb_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  int rc, ret;
  MDB_val key, data;
  MDB_txn *txn;
  mapcache_cache_lmdb *cache = (mapcache_cache_lmdb*)pcache;
  char *skey;

  if (lmdb_env->is_open == 0) {
    ctx->set_error(ctx,500,"lmdb is not open %s",cache->basedir);
    return MAPCACHE_FALSE;
  }

  skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  key.mv_size = strlen(skey)+1;
  key.mv_data = skey;

  rc = mdb_txn_begin(lmdb_env->env, NULL, MDB_RDONLY, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction for has_tile in %s:%s",cache->basedir,mdb_strerror(rc));
    return MAPCACHE_FALSE;
  }

  rc = mdb_get(txn, lmdb_env->dbi, &key, &data);
  if(rc == 0) {
    ret = MAPCACHE_TRUE;
  } else if(rc == MDB_NOTFOUND) {
    ret = MAPCACHE_FALSE;
  } else {
    ctx->set_error(ctx,500,"lmdb failed to get tile for has_tile in %s:%s",cache->basedir,mdb_strerror(rc));
    ret = MAPCACHE_FALSE;
  }

  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction for has_tile in %s:%s",cache->basedir,mdb_strerror(rc));
    ret = MAPCACHE_FALSE;
  }

  return ret;
}

static void _mapcache_cache_lmdb_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  int rc;
  MDB_val key;
  MDB_txn *txn;
  mapcache_cache_lmdb *cache = (mapcache_cache_lmdb*)pcache;
  char *skey;

  if (lmdb_env->is_open == 0) {
    ctx->set_error(ctx,500,"lmdb is not open %s",cache->basedir);
    return;
  }

  skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  key.mv_size = strlen(skey)+1;
  key.mv_data = skey;

  rc = mdb_txn_begin(lmdb_env->env, NULL, 0, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction for delete in %s:%s",cache->basedir,mdb_strerror(rc));
    return;
  }

  rc = mdb_del(txn, lmdb_env->dbi, &key, NULL);
  if (rc) {
    if (rc == MDB_NOTFOUND) {
      ctx->log(ctx,MAPCACHE_DEBUG,"attempt to delete tile %s absent in the db %s",skey,cache->basedir);
    }
    else {
      ctx->set_error(ctx,500,"lmdb failed to delete for tile_delete in %s:%s",cache->basedir,mdb_strerror(rc));
    }
  }

  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction for delete in %s:%s",cache->basedir,mdb_strerror(rc));
  }
}

static int _mapcache_cache_lmdb_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  int rc, ret;
  MDB_val key, data;
  MDB_txn *txn;
  char *skey;
  mapcache_cache_lmdb *cache = (mapcache_cache_lmdb*)pcache;

  if (lmdb_env->is_open == 0) {
    ctx->set_error(ctx,500,"lmdb is not open %s",cache->basedir);
    return MAPCACHE_FALSE;
  }

  skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  key.mv_size = strlen(skey)+1;
  key.mv_data = skey;

  rc = mdb_txn_begin(lmdb_env->env, NULL, MDB_RDONLY, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction for get in %s:%s",cache->basedir,mdb_strerror(rc));
    return MAPCACHE_FALSE;
  }

  rc = mdb_get(txn, lmdb_env->dbi, &key, &data);
  if(rc == 0) {
    if(((char*)(data.mv_data))[0] == '#') {
      tile->encoded_data = mapcache_empty_png_decode(ctx,tile->grid_link->grid->tile_sx, tile->grid_link->grid->tile_sy, (unsigned char*)data.mv_data,&tile->nodata);
    } else {
      tile->encoded_data = mapcache_buffer_create(data.mv_size,ctx->pool);
      memcpy(tile->encoded_data->buf, data.mv_data, data.mv_size);
      tile->encoded_data->size = data.mv_size-sizeof(apr_time_t);
      tile->encoded_data->avail = data.mv_size;
    }
    tile->mtime = *((apr_time_t*)(((char*)data.mv_data)+data.mv_size-sizeof(apr_time_t)));
    ret = MAPCACHE_SUCCESS;
  } else if(rc == MDB_NOTFOUND) {
    ret = MAPCACHE_CACHE_MISS;
  } else {
    ctx->set_error(ctx,500,"lmdb failed for tile_get in %s:%s",cache->basedir,mdb_strerror(rc));
    ret = MAPCACHE_FAILURE;
  }

  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction for tile_get in %s:%s",cache->basedir,mdb_strerror(rc));
    ret = MAPCACHE_FALSE;
  }

  return ret;
}


static void _mapcache_cache_lmdb_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  int rc;
  MDB_val key, data;
  MDB_txn *txn;
  apr_time_t now;
  mapcache_cache_lmdb *cache = (mapcache_cache_lmdb*)pcache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);

  now = apr_time_now();

  key.mv_size = strlen(skey)+1;
  key.mv_data = skey;

  if(!tile->raw_image) {
    tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
    GC_CHECK_ERROR(ctx);
  }

  if(tile->raw_image->h==256 && tile->raw_image->w==256 && mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
    data.mv_size = 5+sizeof(apr_time_t);
    data.mv_data = apr_palloc(ctx->pool,data.mv_size);
    (((char*)data.mv_data)[0])='#';
    memcpy(((char*)data.mv_data)+1,tile->raw_image->data,4);
    memcpy(((char*)data.mv_data)+5,&now,sizeof(apr_time_t));
  } else {
    if(!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
    }
    mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
    data.mv_data = tile->encoded_data->buf;
    data.mv_size = tile->encoded_data->size;
    tile->encoded_data->size -= sizeof(apr_time_t);
  }

  if (lmdb_env->is_open == 0) {
    ctx->set_error(ctx,500,"lmdb is not open %s",cache->basedir);
    return;
  }

  rc = mdb_txn_begin(lmdb_env->env, NULL, 0, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction for set in %s:%s",cache->basedir,mdb_strerror(rc));
    return;
  }

  rc = mdb_put(txn, lmdb_env->dbi, &key, &data, 0);
  if(rc) {
    ctx->set_error(ctx,500,"lmbd failed to put for tile_set in %s:%s",cache->basedir,mdb_strerror(rc));
  }

  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction for tile_set in %s:%s",cache->basedir,mdb_strerror(rc));
  }
}

static void _mapcache_cache_lmdb_multiset(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  int rc;
  MDB_val key, data;
  MDB_txn *txn;
  int i;
  apr_time_t now;
  mapcache_cache_lmdb *cache = (mapcache_cache_lmdb*)pcache;

  now = apr_time_now();

  if (lmdb_env->is_open == 0) {
    ctx->set_error(ctx,500,"lmdb is not open %s",cache->basedir);
    return;
  }

  rc = mdb_txn_begin(lmdb_env->env, NULL, 0, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction for multiset in %s:%s",cache->basedir,mdb_strerror(rc));
    return;
  }

  for(i=0; i<ntiles; i++) {
    char *skey;
    mapcache_tile *tile;
    tile = &tiles[i];
    skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      if(GC_HAS_ERROR(ctx)) {
        goto abort_txn;
      }
    }
    if(tile->raw_image->h==256 && tile->raw_image->w==256 && mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      data.mv_size = 5+sizeof(apr_time_t);
      data.mv_data = apr_palloc(ctx->pool,data.mv_size);
      (((char*)data.mv_data)[0])='#';
      memcpy(((char*)data.mv_data)+1,tile->raw_image->data,4);
      memcpy(((char*)data.mv_data)+5,&now,sizeof(apr_time_t));
    } else {
      if(!tile->encoded_data) {
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        if(GC_HAS_ERROR(ctx)) {
          goto abort_txn;
        }
      }
      mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
      data.mv_data = tile->encoded_data->buf;
      data.mv_size = tile->encoded_data->size;
      tile->encoded_data->size -= sizeof(apr_time_t);
    }
    key.mv_data = skey;
    key.mv_size = strlen(skey)+1;

    rc = mdb_put(txn, lmdb_env->dbi, &key, &data, 0);
    if(rc) {
      ctx->set_error(ctx,500,"lmbd failed to put for multiset in %s:%s",cache->basedir,mdb_strerror(rc));
      goto abort_txn;
    }
  }

  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction for multiset in %s:%s",cache->basedir,mdb_strerror(rc));
  }
  return;

abort_txn:
  mdb_txn_abort(txn);
}


static void _mapcache_cache_lmdb_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_lmdb *dcache = (mapcache_cache_lmdb*)cache;
  int max_size;
  apr_status_t rv;
  if ((cur_node = ezxml_child(node,"base")) != NULL) {
    dcache->basedir = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  if ((cur_node = ezxml_child(node,"key_template")) != NULL) {
    dcache->key_template = apr_pstrdup(ctx->pool,cur_node->txt);
  } else {
    dcache->key_template = apr_pstrdup(ctx->pool,"{tileset}-{grid}-{dim}-{z}-{y}-{x}.{ext}");
  }
  if ((cur_node = ezxml_child(node,"max_size")) != NULL) {
    rv = apr_cstr_atoi(&max_size,cur_node->txt);
    if(rv != APR_SUCCESS) {
      char errmsg[120];
      ctx->set_error(ctx,500,"lmdb cache failed to parse max_size %s:%s",cur_node->txt,apr_strerror(rv,errmsg,120));
      return;
    }
  } else {
    max_size = 64; /* = 250MiB on most systems */
  }
  dcache->max_size = sysconf(_SC_PAGESIZE) * max_size * 1000;
  /* Max_readers defaults to 126 */
  if ((cur_node = ezxml_child(node,"max_readers")) != NULL) {
    unsigned int max_readers;
    rv = apr_cstr_atoui(&max_readers,cur_node->txt);
    if(rv != APR_SUCCESS) {
      char errmsg[120];
      ctx->set_error(ctx,500,"lmdb cache failed to parse max_readers %s:%s",cur_node->txt,apr_strerror(rv,errmsg,120));
      return;
    }
    dcache->max_readers = max_readers;
  }
  if(!dcache->basedir) {
    ctx->set_error(ctx,500,"lmbd cache \"%s\" is missing <base> entry",cache->name);
    return;
  }
}

/**
 * \private \memberof mapcache_cache_lmdb
 */
static void _mapcache_cache_lmdb_configuration_post_config(mapcache_context *ctx,
    mapcache_cache *cache, mapcache_cfg *cfg)
{
  mapcache_cache_lmdb *dcache = (mapcache_cache_lmdb*)cache;
  apr_status_t rv;
  apr_dir_t *dir;

  rv = apr_dir_open(&dir, dcache->basedir, ctx->pool);
  if(rv != APR_SUCCESS) {
    char errmsg[120];
    ctx->set_error(ctx,500,"lmdb failed to open directory %s:%s",dcache->basedir,apr_strerror(rv,errmsg,120));
    return;
  }
}

static void _mapcache_cache_lmdb_child_init(mapcache_context *ctx, mapcache_cache *cache, apr_pool_t *pchild)
{
  mapcache_cache_lmdb *dcache = (mapcache_cache_lmdb*)cache;

  int rc, dead=0;
  MDB_txn *txn;

  lmdb_env_s *var = apr_pcalloc(ctx->pool,sizeof(lmdb_env_s));
  lmdb_env = var;
  lmdb_env->is_open = 0;
  rc = mdb_env_create(&(lmdb_env->env));
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to create environment of database %s:%s",dcache->basedir,mdb_strerror(rc));
    return;
  }
  rc = mdb_env_set_mapsize(lmdb_env->env, dcache->max_size);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to set maximum size of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_env_close(lmdb_env->env);
    return;
  }
  if (dcache->max_readers) {
    rc = mdb_env_set_maxreaders(lmdb_env->env, dcache->max_readers);
    if (rc) {
      ctx->set_error(ctx,500,"lmdb failed to set maximum readers of database %s:%s",dcache->basedir,mdb_strerror(rc));
      mdb_env_close(lmdb_env->env);
      return;
    }
  }
  /* Clean out any stale reader entries from lock table */
  rc = mdb_reader_check(lmdb_env->env, &dead);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to clear stale readers of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_env_close(lmdb_env->env);
    return;
  }
  if (dead) {
    ctx->log(ctx,MAPCACHE_NOTICE,"lmdb cleared %d stale readers of database %s",dead,dcache->basedir);
  }
  rc = mdb_env_open(lmdb_env->env, dcache->basedir, 0, 0664);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to open environment of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_env_close(lmdb_env->env);
    return;
  }
  rc = mdb_txn_begin(lmdb_env->env, NULL, MDB_CREATE, &txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to begin transaction of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_env_close(lmdb_env->env);
    return;
  }
  rc = mdb_dbi_open(txn, NULL, 0, &(lmdb_env->dbi));
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to open dbi of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(lmdb_env->env);
    return;
  }
  rc = mdb_txn_commit(txn);
  if (rc) {
    ctx->set_error(ctx,500,"lmdb failed to commit transaction of database %s:%s",dcache->basedir,mdb_strerror(rc));
    mdb_dbi_close(lmdb_env->env, lmdb_env->dbi);
    mdb_env_close(lmdb_env->env);
    return;
  }
  lmdb_env->is_open = 1;
}

/**
 * \brief creates and initializes a mapcache_dbd_cache
 */
mapcache_cache* mapcache_cache_lmdb_create(mapcache_context *ctx)
{
  mapcache_cache_lmdb *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_lmdb));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate LMDB cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_LMDB;
  cache->cache._tile_delete = _mapcache_cache_lmdb_delete;
  cache->cache._tile_get = _mapcache_cache_lmdb_get;
  cache->cache._tile_exists = _mapcache_cache_lmdb_has_tile;
  cache->cache._tile_set = _mapcache_cache_lmdb_set;
  cache->cache._tile_multi_set = _mapcache_cache_lmdb_multiset;
  cache->cache.configuration_post_config = _mapcache_cache_lmdb_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_lmdb_configuration_parse_xml;
  cache->cache.child_init = _mapcache_cache_lmdb_child_init;
  cache->basedir = NULL;
  cache->key_template = NULL;
  cache->max_readers = 0;
  return (mapcache_cache*)cache;
}

#else
mapcache_cache* mapcache_cache_lmdb_create(mapcache_context *ctx)
{
  ctx->set_error(ctx,400,"LMDB support not compiled in this version");
  return NULL;
}
#endif

/* vim: ts=2 sts=2 et sw=2
*/
