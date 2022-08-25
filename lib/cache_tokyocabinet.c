/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Tokyo Cabinet cache backend
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
#ifdef USE_TC

#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_file_info.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <tcutil.h>
#include <tcbdb.h>

#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct mapcache_cache_tc mapcache_cache_tc;
struct mapcache_cache_tc {
  mapcache_cache cache;
  char *basedir;
  char *key_template;
  mapcache_context *ctx;
};
mapcache_cache *mapcache_cache_tc_create(mapcache_context *ctx);

struct tc_conn {
  TCBDB *bdb;
  int readonly;
};
static struct tc_conn _tc_get_conn(mapcache_context *ctx, mapcache_tile* tile, int readonly) {
  struct tc_conn conn;
  /* create the object */
  conn.bdb = tcbdbnew();
  mapcache_cache_tc *cache = (mapcache_cache_tc*)tile->tileset->cache;

  /* open the database */
  if(!readonly) {
    if(!tcbdbopen(conn.bdb, apr_pstrcat(ctx->pool, cache->basedir,"/tc.tcb",NULL), BDBOWRITER | BDBOCREAT)) {
      int ecode = tcbdbecode(conn.bdb);
      ctx->set_error(ctx,500, "tokyocabinet open error on %s: %s\n",apr_pstrcat(ctx->pool, cache->basedir,"/tc.tcf",NULL),tcbdberrmsg(ecode));
    }
    conn.readonly = 0;
  } else {
    if(!tcbdbopen(conn.bdb, apr_pstrcat(ctx->pool, cache->basedir,"/tc.tcb",NULL), BDBOREADER)) {
      if(!tcbdbopen(conn.bdb, apr_pstrcat(ctx->pool, cache->basedir,"/tc.tcb",NULL), BDBOWRITER | BDBOCREAT)) {
        int ecode = tcbdbecode(conn.bdb);
        ctx->set_error(ctx,500, "tokyocabinet open error on %s: %s\n",apr_pstrcat(ctx->pool, cache->basedir,"/tc.tcf",NULL),tcbdberrmsg(ecode));
      }
      conn.readonly = 0;
    }
    conn.readonly = 1;
  }
  return conn;
}

static void _tc_release_conn(mapcache_context *ctx, mapcache_tile *tile, struct tc_conn conn)
{
  if(!conn.readonly)
    tcbdbsync(conn.bdb);

  if(!tcbdbclose(conn.bdb)) {
    int ecode = tcbdbecode(conn.bdb);
    ctx->set_error(ctx,500, "tokyocabinet close error: %s\n",tcbdberrmsg(ecode));
  }
  tcbdbdel(conn.bdb);
}

static int _mapcache_cache_tc_has_tile(mapcache_context *ctx, mapcache_tile *tile)
{
  int ret;
  struct tc_conn conn;
  int nrecords = 0;
  mapcache_cache_tc *cache = (mapcache_cache_tc*)tile->tileset->cache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  conn = _tc_get_conn(ctx,tile,1);
  if(GC_HAS_ERROR(ctx)) return MAPCACHE_FALSE;
  nrecords = tcbdbvnum2(conn.bdb, skey);
  if(nrecords == 0)
    ret = MAPCACHE_FALSE;
  else
    ret = MAPCACHE_TRUE;
  _tc_release_conn(ctx,tile,conn);
  return ret;
}

static void _mapcache_cache_tc_delete(mapcache_context *ctx, mapcache_tile *tile)
{
  struct tc_conn conn;
  mapcache_cache_tc *cache = (mapcache_cache_tc*)tile->tileset->cache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  conn = _tc_get_conn(ctx,tile,0);
  GC_CHECK_ERROR(ctx);
  tcbdbout2(conn.bdb, skey);
  _tc_release_conn(ctx,tile,conn);
}


static int _mapcache_cache_tc_get(mapcache_context *ctx, mapcache_tile *tile)
{
  int ret;
  struct tc_conn conn;
  mapcache_cache_tc *cache = (mapcache_cache_tc*)tile->tileset->cache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  conn = _tc_get_conn(ctx,tile,1);
  int size;
  if(GC_HAS_ERROR(ctx)) return MAPCACHE_FAILURE;
  tile->encoded_data = mapcache_buffer_create(0,ctx->pool);
  tile->encoded_data->buf = tcbdbget(conn.bdb, skey, strlen(skey), &size);
  if(tile->encoded_data->buf) {
    tile->encoded_data->avail = size;
    tile->encoded_data->size = size - sizeof(apr_time_t);
    apr_pool_cleanup_register(ctx->pool, tile->encoded_data->buf,(void*)free, apr_pool_cleanup_null);
    tile->mtime = *((apr_time_t*)(&tile->encoded_data->buf[tile->encoded_data->size]));
    ret = MAPCACHE_SUCCESS;
  } else {
    ret = MAPCACHE_CACHE_MISS;
  }
  _tc_release_conn(ctx,tile,conn);
  return ret;
}

static void _mapcache_cache_tc_set(mapcache_context *ctx, mapcache_tile *tile)
{
  struct tc_conn conn;
  mapcache_cache_tc *cache = (mapcache_cache_tc*)tile->tileset->cache;
  char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
  apr_time_t now = apr_time_now();
  conn = _tc_get_conn(ctx,tile,0);
  GC_CHECK_ERROR(ctx);

  if(!tile->encoded_data) {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }
  mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
  if(!tcbdbput(conn.bdb, skey, strlen(skey), tile->encoded_data->buf, tile->encoded_data->size)) {
    int ecode = tcbdbecode(conn.bdb);
    ctx->set_error(ctx,500, "tokyocabinet put error: %s\n",tcbdberrmsg(ecode));
  }
  _tc_release_conn(ctx,tile,conn);
}


static void _mapcache_cache_tc_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_tc *dcache = (mapcache_cache_tc*)cache;
  if ((cur_node = ezxml_child(node,"base")) != NULL) {
    dcache->basedir = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  if ((cur_node = ezxml_child(node,"key_template")) != NULL) {
    dcache->key_template = apr_pstrdup(ctx->pool,cur_node->txt);
  } else {
    dcache->key_template = apr_pstrdup(ctx->pool,"{tileset}-{grid}-{dim}-{z}-{y}-{x}.{ext}");
  }
  if(!dcache->basedir) {
    ctx->set_error(ctx,500,"tokyocabinet cache \"%s\" is missing <base> entry",cache->name);
    return;
  }
}

static void _mapcache_cache_tc_configuration_post_config(mapcache_context *ctx,
    mapcache_cache *cache, mapcache_cfg *cfg)
{
  mapcache_cache_tc *dcache = (mapcache_cache_tc*)cache;
  apr_status_t rv;
  apr_dir_t *dir;
  rv = apr_dir_open(&dir, dcache->basedir, ctx->pool);
  if(rv != APR_SUCCESS) {
    char errmsg[120];
    ctx->set_error(ctx,500,"bdb failed to open directory %s:%s",dcache->basedir,apr_strerror(rv,errmsg,120));
  }
}

static void _mapcache_cache_tc_child_init(mapcache_cache *cache, apr_pool_t *pchild) {
};

/**
 * \brief creates and initializes a mapcache_dbd_cache
 */
mapcache_cache* mapcache_cache_tc_create(mapcache_context *ctx)
{
  mapcache_cache_tc *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_tc));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate tokyo cabinet cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_TC;
  cache->cache.tile_delete = _mapcache_cache_tc_delete;
  cache->cache.tile_get = _mapcache_cache_tc_get;
  cache->cache.tile_exists = _mapcache_cache_tc_has_tile;
  cache->cache.tile_set = _mapcache_cache_tc_set;
  cache->cache.configuration_post_config = _mapcache_cache_tc_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_tc_configuration_parse_xml;
  cache->cache.child_init = _mapcache_cache_tc_child_init;
  cache->basedir = NULL;
  cache->key_template = NULL;
  return (mapcache_cache*)cache;
}

#else

mapcache_cache* mapcache_cache_tc_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"TokyoCabinet support not compiled in this version");
  return NULL;
}

#endif

/* vim: ts=2 sts=2 et sw=2
*/
