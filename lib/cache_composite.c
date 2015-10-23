/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: composite cache backend.
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without compositeriction, including without limitation
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

static mapcache_cache_composite_cache_link* _mapcache_cache_link_create(apr_pool_t *pool) {
  mapcache_cache_composite_cache_link *cl = apr_pcalloc(pool, sizeof(mapcache_cache_composite_cache_link));
  cl->cache=NULL;
  cl->dimensions=NULL;
  cl->grids=NULL;
  cl->maxzoom=-1;
  cl->minzoom=-1;
  return cl;
}
/**
 * returns the mapcache_cache to use for a given tile
 * @param ctx
 * @param tile
 * @return 
 */
static mapcache_cache* _mapcache_composite_cache_get(mapcache_context *ctx, mapcache_cache_composite *cache, mapcache_tile *tile) {
  int i;
  for(i=0; i<cache->cache_links->nelts; i++) {
    mapcache_cache_composite_cache_link *cache_link = APR_ARRAY_IDX(cache->cache_links,i,mapcache_cache_composite_cache_link*);
    if(cache_link->minzoom != -1 && tile->z < cache_link->minzoom) continue;
    if(cache_link->maxzoom != -1 && tile->z > cache_link->maxzoom) continue;
    if(cache_link->grids) {
      int j;
      for(j=0;j<cache_link->grids->nelts;j++) {
        char *grid_name = APR_ARRAY_IDX(cache_link->grids,j,char*);
        if(!strcmp(tile->grid_link->grid->name,grid_name))
          break;
      }
      /* not found */
      if(j == cache_link->grids->nelts) continue;
    }
    if(cache_link->dimensions) {
      const apr_array_header_t *array = apr_table_elts(cache_link->dimensions);
      apr_table_entry_t *elts = (apr_table_entry_t *) array->elts;
      int j;
      if(!tile->dimensions) continue; /* the cache link refers to dimensions, but this tile does not have any, it cannot match */
      
      for (j = 0; j < array->nelts; j++) {
        char *dim = elts[j].key;
        char *dimval = elts[j].val;
        int k;
        for(k=0;k<tile->dimensions->nelts;k++) {
          mapcache_requested_dimension *rdim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
          if(!strcmp(rdim->dimension->name,dim) && !strcmp(rdim->cached_value,dimval))
            break;
        }
        if(k == tile->dimensions->nelts) break; /* no tile dimension matched the current cache dimension */
      }
      if(j != array->nelts) continue; /* we broke out early from the cache dimension loop, so at least one was not correct */
    }
    return cache_link->cache;
  }
  ctx->set_error(ctx, 500, "no cache matches for given tile request");
  return NULL;
}

static int _mapcache_cache_composite_tile_exists(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  mapcache_cache *subcache;
  subcache = _mapcache_composite_cache_get(ctx, cache, tile);
  if(GC_HAS_ERROR(ctx) || !subcache)
    return MAPCACHE_FAILURE;
  return subcache->tile_exists(ctx, subcache, tile);
}

static void _mapcache_cache_composite_tile_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  mapcache_cache *subcache;
  subcache = _mapcache_composite_cache_get(ctx, cache, tile);
  GC_CHECK_ERROR(ctx);
  /*delete the tile itself*/
  subcache->tile_delete(ctx,subcache,tile);
}

/**
 * \brief get content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored on the composite server
 * \private \memberof mapcache_cache_composite
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_composite_tile_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  mapcache_cache *subcache;
  subcache = _mapcache_composite_cache_get(ctx, cache, tile);
  GC_CHECK_ERROR_RETURN(ctx);
  return subcache->tile_get(ctx,subcache,tile);
}

static void _mapcache_cache_composite_tile_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  mapcache_cache *subcache;
  subcache = _mapcache_composite_cache_get(ctx, cache, tile);
  GC_CHECK_ERROR(ctx);
  return subcache->tile_set(ctx,subcache,tile);
}

static void _mapcache_cache_composite_tile_multi_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tiles, int ntiles)
{
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  mapcache_cache *subcache;
  subcache = _mapcache_composite_cache_get(ctx, cache, &tiles[0]);
  GC_CHECK_ERROR(ctx);
  if(subcache->tile_multi_set) {
    return subcache->tile_multi_set(ctx,subcache,tiles,ntiles);
  } else {
    int i;
    for(i=0; i<ntiles; i++) {
      subcache->tile_set(ctx, subcache, &tiles[i]);
    }
  }
}

/**
 * \private \memberof mapcache_cache_composite
 */
static void _mapcache_cache_composite_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *pcache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_composite *cache = (mapcache_cache_composite*)pcache;
  cache->cache_links = apr_array_make(ctx->pool,3,sizeof(mapcache_cache_composite_cache_link*));
  for(cur_node = ezxml_child(node,"cache"); cur_node; cur_node = cur_node->next) {
    char *sZoom;
    int zoom;
    mapcache_cache *refcache = mapcache_configuration_get_cache(config, cur_node->txt);
    mapcache_cache_composite_cache_link *cachelink;
    if(!refcache) {
      ctx->set_error(ctx, 400, "composite cache \"%s\" references cache \"%s\","
                     " but it is not configured (hint:referenced caches must be declared before this composite cache in the xml file)", pcache->name, cur_node->txt);
      return;
    }
    cachelink = _mapcache_cache_link_create(ctx->pool);
    cachelink->cache = refcache;

    sZoom = (char*)ezxml_attr(cur_node,"max-zoom");
    if(sZoom) {
      char *endptr;
      zoom = (int)strtol(sZoom,&endptr,10);
      if(*endptr != 0 || zoom < 0) {
        ctx->set_error(ctx, 400, "failed to parse cache max-zoom %s (expecting a positive integer)",
                       sZoom);
        return;
      }
      cachelink->maxzoom = zoom;
    }
    sZoom = (char*)ezxml_attr(cur_node,"min-zoom");
    if(sZoom) {
      char *endptr;
      zoom = (int)strtol(sZoom,&endptr,10);
      if(*endptr != 0 || zoom < 0) {
        ctx->set_error(ctx, 400, "failed to parse cache min-zoom %s (expecting a positive integer)",
                       sZoom);
        return;
      }
      cachelink->minzoom = zoom;
    }
    sZoom = (char*)ezxml_attr(cur_node,"grids");
    if(sZoom) {
      char *grids = apr_pstrdup(ctx->pool,sZoom),*key,*last;
      for(key = apr_strtok(grids, ",", &last); key; key = apr_strtok(NULL,",",&last)) {
        /*loop through grids*/
        if(!cachelink->grids) {
          cachelink->grids =apr_array_make(ctx->pool,1,sizeof(char*));
        }
        APR_ARRAY_PUSH(cachelink->grids,char*) = key;
      }
    }
    sZoom = (char*)ezxml_attr(cur_node,"dimensions");
    if(sZoom) {
      char *dims = apr_pstrdup(ctx->pool,sZoom),*key,*last;
      for(key = apr_strtok(dims, ",", &last); key; key = apr_strtok(NULL,",",&last)) {
        char *dimname;
        /*loop through dims*/
        if(!cachelink->dimensions) {
          cachelink->dimensions =apr_table_make(ctx->pool,1);
        }
        dimname = key;
        while(*key && *key!='=') key++;
        if(!(*key)) {
          ctx->set_error(ctx,400,"failed to parse composite cache dimensions. expecting dimensions=\"dim1=val1,dim2=val2\"");
          return;
        }
        *key = 0;
        key++;
        apr_table_set(cachelink->dimensions,dimname,key);
      }
    }
    
    APR_ARRAY_PUSH(cache->cache_links,mapcache_cache_composite_cache_link*) = cachelink;
  }
}

/**
 * \private \memberof mapcache_cache_composite
 */
static void _mapcache_cache_composite_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
}


/**
 * \brief creates and initializes a mapcache_cache_composite
 */
mapcache_cache* mapcache_cache_composite_create(mapcache_context *ctx)
{
  mapcache_cache_composite *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_composite));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate composite cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_COMPOSITE;
  cache->cache.tile_delete = _mapcache_cache_composite_tile_delete;
  cache->cache.tile_get = _mapcache_cache_composite_tile_get;
  cache->cache.tile_exists = _mapcache_cache_composite_tile_exists;
  cache->cache.tile_set = _mapcache_cache_composite_tile_set;
  cache->cache.tile_multi_set = _mapcache_cache_composite_tile_multi_set;
  cache->cache.configuration_post_config = _mapcache_cache_composite_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_composite_configuration_parse_xml;
  return (mapcache_cache*)cache;
}
