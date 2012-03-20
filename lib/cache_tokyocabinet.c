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

#ifdef USE_TC

#include "mapcache.h"
#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_file_info.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif



static const char* _tc_get_tile_dimkey(mapcache_context *ctx, mapcache_tile *tile) {
   if(tile->dimensions) {
      const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
      int i = elts->nelts;
      if(i>1) {
         char *key = "";
         while(i--) {
            apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
            if(i) {
               key = apr_pstrcat(ctx->pool,key,entry->val,"#",NULL);
            } else {
               key = apr_pstrcat(ctx->pool,key,entry->val,NULL);
            }
         }
         return key;
      } else if(i){
         apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,0,apr_table_entry_t));
         return entry->val;
      } else {
         return "";
      }
   } else {
      return "";
   }
}

static char* _tc_get_key(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*) tile->tileset->cache;
   char *path = cache->key_template;
   path = mapcache_util_str_replace(ctx->pool, path, "{x}",
           apr_psprintf(ctx->pool, "%d", tile->x));
   path = mapcache_util_str_replace(ctx->pool, path, "{y}",
           apr_psprintf(ctx->pool, "%d", tile->y));
   path = mapcache_util_str_replace(ctx->pool, path, "{z}",
           apr_psprintf(ctx->pool, "%d", tile->z));
   if(strstr(path,"{dim}")) {
      path = mapcache_util_str_replace(ctx->pool, path, "{dim}", _bdb_get_tile_dimkey(ctx,tile));
   }
   if(strstr(path,"{tileset}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{tileset}", tile->tileset->name);
   if(strstr(path,"{grid}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{grid}", tile->grid_link->grid->name);
   if(strstr(path,"{ext}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{ext}",
           tile->tileset->format ? tile->tileset->format->extension : "png");
   return path;
}


static int _mapcache_cache_tc_has_tile(mapcache_context *ctx, mapcache_tile *tile) {
}

static void _mapcache_cache_tc_delete(mapcache_context *ctx, mapcache_tile *tile) {
}


static int _mapcache_cache_tc_get(mapcache_context *ctx, mapcache_tile *tile) {
}

static void _mapcache_cache_tc_set(mapcache_context *ctx, mapcache_tile *tile) {
}


static void _mapcache_cache_tc_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
}
   
static void _mapcache_cache_tc_configuration_post_config(mapcache_context *ctx,
      mapcache_cache *cache, mapcache_cfg *cfg) {
}

/**
 * \brief creates and initializes a mapcache_dbd_cache
 */
mapcache_cache* mapcache_cache_tc_create(mapcache_context *ctx) {
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
   cache->basedir = NULL;
   cache->key_template = NULL;
   return (mapcache_cache*)cache;
}

#endif

/* vim: ai ts=3 sts=3 et sw=3
*/
