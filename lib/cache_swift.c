/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: swift cache backend.
 * Author:   Fabian Shindler <fabian.schindler@eox.at>
 *
 ******************************************************************************
 * Copyright (c) 1996-2019 EOX IT Services GmbH
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
#ifdef USE_SWIFT

#include "keystone-client.h"
#include "swift-client.h"

#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_hash.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>

typedef struct mapcache_cache_swift mapcache_cache_swift;

/**\class mapcache_cache_swift
 * \brief a mapcache_cache for openstack Swift object storages using keystone authentication
 * \implements mapcache_cache
 */
struct mapcache_cache_swift {
   mapcache_cache cache;
   char *auth_url;
   char *tenant;
   char *username;
   char *password;
   char *key_template;
   char *container_template;
   int debug;
   enum keystone_auth_version auth_version;
};

struct swift_conn_params {
  mapcache_cache_swift *cache;
};


struct swift_connection {
   keystone_context_t *keystone_context;
   swift_context_t *swift_context;
};

void mapcache_swift_authenticate(mapcache_context *ctx, mapcache_cache_swift *cache, struct swift_connection *conn) {
    enum keystone_error keystone_err;
    enum swift_error swift_err;
    char *url;
    char *token;

    keystone_err = keystone_authenticate(conn->keystone_context, cache->auth_url, cache->tenant, cache->username, cache->password);
    if (keystone_err != KSERR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to keystone_authenticate()");
        return;
    }

    token = (char *)keystone_get_auth_token(conn->keystone_context);
    if (token == NULL) {
        ctx->set_error(ctx, 500, "failed to keystone_get_auth_token()");
        return;
    }

    swift_err = swift_set_auth_token(conn->swift_context, token);
    if (swift_err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to swift_set_auth_token()");
        return;
    }

    // TODO: get version and endpoint type
    url = (char *)keystone_get_service_url(conn->keystone_context, OS_SERVICE_SWIFT, 1, OS_ENDPOINT_URL_PRIVATE);
    if (token == NULL) {
        ctx->set_error(ctx, 500, "failed to keystone_get_service_url()");
        return;
    }

    swift_err = swift_set_url(conn->swift_context, url);
    if (swift_err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to swift_set_url()");
        return;
    }
}

void mapcache_swift_connection_constructor(mapcache_context *ctx, void **conn_, void *params) {
    enum keystone_error keystone_err;
    enum swift_error swift_err;
    struct swift_connection *conn;

    mapcache_cache_swift *cache = ((struct swift_conn_params*)params)->cache;

    conn = calloc(sizeof(struct swift_connection), 1);
    conn->keystone_context = calloc(sizeof(keystone_context_t), 1);
    conn->swift_context = calloc(sizeof(swift_context_t), 1);

    keystone_err = keystone_start(conn->keystone_context);
    if (keystone_err != KSERR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to keystone_start()");
        return;
    }

    keystone_err = keystone_set_auth_version(conn->keystone_context, cache->auth_version);
    if (keystone_err != KSERR_SUCCESS) {
        keystone_end(conn->keystone_context);
        ctx->set_error(ctx, 500, "failed to keystone_set_auth_version()");
        return;
    }

    swift_err = swift_start(conn->swift_context);
    if (swift_err != SCERR_SUCCESS) {
        keystone_end(conn->keystone_context);
        ctx->set_error(ctx, 500, "failed to swift_start()");
        return;
    }

    if (cache->debug) {
        keystone_err = keystone_set_debug(conn->keystone_context, cache->debug);
        if (keystone_err != KSERR_SUCCESS) {
            keystone_end(conn->keystone_context);
            swift_end(conn->swift_context);
            ctx->set_error(ctx, 500, "failed to keystone_set_debug()");
            return;
        }
        swift_err = swift_set_debug(conn->swift_context, cache->debug);
        if (swift_err != SCERR_SUCCESS) {
            keystone_end(conn->keystone_context);
            swift_end(conn->swift_context);
            ctx->set_error(ctx, 500, "failed to swift_set_debug()");
            return;
        }
    }

    mapcache_swift_authenticate(ctx, cache, conn);
    if (GC_HAS_ERROR(ctx)) {
        keystone_end(conn->keystone_context);
        swift_end(conn->swift_context);
        return;
    }

    *conn_ = conn;
}

void mapcache_swift_connection_destructor(void *conn_) {
    struct swift_connection *conn = (struct swift_connection *)conn_;

    keystone_end(conn->keystone_context);
    swift_end(conn->swift_context);
    free(conn->keystone_context);
    free(conn->swift_context);
    free(conn);
}

static mapcache_pooled_connection* _swift_get_connection(mapcache_context *ctx, mapcache_cache_swift *cache, mapcache_tile *tile)
{
  mapcache_pooled_connection *pc;
  struct swift_conn_params params;

  params.cache = cache;

  pc = mapcache_connection_pool_get_connection(ctx,cache->cache.name,mapcache_swift_connection_constructor,
          mapcache_swift_connection_destructor, &params);

  return pc;
}

static int _mapcache_cache_swift_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    mapcache_pooled_connection *pc;
    mapcache_cache_swift *cache = (mapcache_cache_swift*) pcache;
    struct swift_connection *conn;
    enum swift_error err;
    int exists;
    int rv = MAPCACHE_FALSE;
    char *container;
    char *key;

    key = mapcache_util_get_tile_key(ctx, tile, cache->key_template, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FALSE;
    }

    if(strchr(cache->container_template,'{')) {
      container = mapcache_util_get_tile_key(ctx, tile, cache->container_template, " \r\n\t\f\e\a\b", "#");
    } else {
      container = cache->container_template;
    }

    pc = _swift_get_connection(ctx, cache, tile);

    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FALSE;
    }
    conn = pc->connection;

    err = swift_set_container(conn->swift_context, container);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set container %s: %d", container, err);
        goto cleanup;
    }
    err = swift_set_object(conn->swift_context, key);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set object %s: %d", key, err);
        goto cleanup;
    }

    err = swift_has(conn->swift_context, &exists);
    if (err == SCERR_AUTH_FAILED) {
        /* re-authenticate and retry */
        mapcache_swift_authenticate(ctx, cache, conn);
        if (GC_HAS_ERROR(ctx)) {
            goto cleanup;
        }
        err = swift_has(conn->swift_context, &exists);
    }

    if (err == SCERR_SUCCESS && exists) {
        rv = MAPCACHE_TRUE;
    } else {
        rv = MAPCACHE_FALSE;
    }

cleanup:
    mapcache_connection_pool_release_connection(ctx, pc);
    return rv;
}

static void _mapcache_cache_swift_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    mapcache_pooled_connection *pc;
    mapcache_cache_swift *cache = (mapcache_cache_swift*) pcache;
    struct swift_connection *conn;
    enum swift_error err;
    char *container;
    char *key;

    key = mapcache_util_get_tile_key(ctx, tile, cache->key_template, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return;
    }

    if(strchr(cache->container_template,'{')) {
      container = mapcache_util_get_tile_key(ctx, tile, cache->container_template, " \r\n\t\f\e\a\b", "#");
    } else {
      container = cache->container_template;
    }

    pc = _swift_get_connection(ctx, cache, tile);

    if (GC_HAS_ERROR(ctx)) {
        return;
    }
    conn = pc->connection;

    err = swift_set_container(conn->swift_context, container);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set container %s: %d", container, err);
        goto cleanup;
    }
    err = swift_set_object(conn->swift_context, key);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set object %s: %d", key, err);
        goto cleanup;
    }

    err = swift_delete_object(conn->swift_context);
    if (err == SCERR_AUTH_FAILED) {
        mapcache_swift_authenticate(ctx, cache, conn);
        if (GC_HAS_ERROR(ctx)) {
            goto cleanup;
        }
        err = swift_delete_object(conn->swift_context);
    } else if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to delete object %s: %d", key, err);
    }

cleanup:
    mapcache_connection_pool_release_connection(ctx, pc);
    return;
}

/**
 * \brief get content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored on the swift object storage
 * \private \memberof mapcache_cache_swift
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_swift_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    mapcache_pooled_connection *pc;
    mapcache_cache_swift *cache = (mapcache_cache_swift*) pcache;
    struct swift_connection *conn;
    enum swift_error err;
    int rv = MAPCACHE_FAILURE;
    char *container;
    char *key;
    size_t size;
    void *data = NULL;

    key = mapcache_util_get_tile_key(ctx, tile, cache->key_template, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FAILURE;
    }

    if(strchr(cache->container_template,'{')) {
      container = mapcache_util_get_tile_key(ctx, tile, cache->container_template, " \r\n\t\f\e\a\b", "#");
    } else {
      container = cache->container_template;
    }

    pc = _swift_get_connection(ctx, cache, tile);

    if (GC_HAS_ERROR(ctx)) {
        return MAPCACHE_FAILURE;
    }
    conn = pc->connection;

    err = swift_set_container(conn->swift_context, container);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set container %s: %d", container, err);
        goto cleanup;
    }
    err = swift_set_object(conn->swift_context, key);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set object %s: %d", key, err);
        goto cleanup;
    }

    err = swift_get_data(conn->swift_context, &size, &data);
    if (err == SCERR_AUTH_FAILED) {
        mapcache_swift_authenticate(ctx, cache, conn);
        if (GC_HAS_ERROR(ctx)) {
            goto cleanup;
        }
        err = swift_get_data(conn->swift_context, &size, &data);
    }

    tile->encoded_data = NULL;

    if (err == SCERR_SUCCESS) {
        rv = MAPCACHE_SUCCESS;
        tile->encoded_data = mapcache_buffer_create(0, ctx->pool);
        mapcache_buffer_append(tile->encoded_data, size, data);
    } else if (err == SCERR_NOT_FOUND) {
        /* simply not found, but no error */
        rv = MAPCACHE_CACHE_MISS;
    } else {
        ctx->set_error(ctx, 500, "swift: failed to get object data %s: %d", key, err);
        rv = MAPCACHE_FAILURE;
    }

cleanup:
    conn->swift_context->allocator(data, 0);

    if(GC_HAS_ERROR(ctx)) {
        mapcache_connection_pool_invalidate_connection(ctx, pc);
    } else {
        mapcache_connection_pool_release_connection(ctx, pc);
    }

    return rv;
}

/**
 * \brief push tile data to swift
 *
 * writes the content of mapcache_tile::data to the configured swift object storage
 * \private \memberof mapcache_cache_swift
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_swift_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
    mapcache_pooled_connection *pc;
    mapcache_cache_swift *cache = (mapcache_cache_swift*) pcache;
    struct swift_connection *conn;
    enum swift_error err;
    char *container;
    char *key;

    key = mapcache_util_get_tile_key(ctx, tile, cache->key_template, " \r\n\t\f\e\a\b", "#");
    if (GC_HAS_ERROR(ctx)) {
        return;
    }

    if(strchr(cache->container_template,'{')) {
      container = mapcache_util_get_tile_key(ctx, tile, cache->container_template, " \r\n\t\f\e\a\b", "#");
    } else {
      container = cache->container_template;
    }

    if (!tile->encoded_data) {
        tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
        GC_CHECK_ERROR(ctx);
    }

    pc = _swift_get_connection(ctx, cache, tile);

    if (GC_HAS_ERROR(ctx)) {
        return;
    }
    conn = pc->connection;

    err = swift_set_container(conn->swift_context, container);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set container %s: %d", container, err);
        goto cleanup;
    }
    err = swift_set_object(conn->swift_context, key);
    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "swift: failed to set object %s: %d", key, err);
        goto cleanup;
    }

    err = swift_put_data(conn->swift_context, tile->encoded_data->buf, tile->encoded_data->size);
    if (err == SCERR_AUTH_FAILED) {
        mapcache_swift_authenticate(ctx, cache, conn);
        if (GC_HAS_ERROR(ctx)) {
            goto cleanup;
        }
        err = swift_put_data(conn->swift_context, tile->encoded_data->buf, tile->encoded_data->size);
    }

    if (err != SCERR_SUCCESS) {
        ctx->set_error(ctx, 500, "failed to store tile %s to cache %s due to error %d.", key, cache->cache.name, err);
    }

cleanup:
    mapcache_connection_pool_release_connection(ctx, pc);
}

/**
 * \private \memberof mapcache_cache_swift
 */
static void _mapcache_cache_swift_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *pcache, mapcache_cfg *config) {
    ezxml_t xauth_url,xauth_version,xtenant,xusername,xpassword,xcontainer,xkey,xdebug;
    mapcache_cache_swift *cache = (mapcache_cache_swift *)pcache;

    xauth_url = ezxml_child(node, "auth_url");
    xauth_version = ezxml_child(node, "auth_version");
    xtenant = ezxml_child(node, "tenant");
    xusername = ezxml_child(node, "username");
    xpassword = ezxml_child(node, "password");
    xcontainer = ezxml_child(node, "container");
    xkey = ezxml_child(node, "key");
    xdebug = ezxml_child(node, "debug");

    if (!xauth_url || !xauth_url->txt || ! *xauth_url->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <auth_url>", pcache->name);
        return;
    } else {
        cache->auth_url = apr_pstrdup(ctx->pool, xauth_url->txt);
    }

    if (!xauth_version || !xauth_version->txt || ! *xauth_version->txt) {
        cache->auth_version = KS_AUTH_V1;
    } else {
        if (strcasecmp(xauth_version->txt, "1") == 0 || strcasecmp(xauth_version->txt, "v1") == 0) {
            cache->auth_version = KS_AUTH_V1;
        } else if (strcasecmp(xauth_version->txt, "3") == 0 || strcasecmp(xauth_version->txt, "v3") == 0) {
            cache->auth_version = KS_AUTH_V3;
        } else {
            ctx->set_error(ctx, 400, "cache %s: invalid <auth_version>", pcache->name);
            return;
        }
    }

    if (!xtenant || !xtenant->txt || ! *xtenant->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <tenant>", pcache->name);
        return;
    } else {
        cache->tenant = apr_pstrdup(ctx->pool, xtenant->txt);
    }

    if (!xusername || !xusername->txt || ! *xusername->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <username>", pcache->name);
        return;
    } else {
        cache->username = apr_pstrdup(ctx->pool, xusername->txt);
    }

    if (!xpassword || !xpassword->txt || ! *xpassword->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <password>", pcache->name);
        return;
    } else {
        cache->password = apr_pstrdup(ctx->pool, xpassword->txt);
    }

    if (!xcontainer || !xcontainer->txt || ! *xcontainer->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <container>", pcache->name);
        return;
    } else {
        cache->container_template = apr_pstrdup(ctx->pool, xcontainer->txt);
    }

    if (!xkey || !xkey->txt || ! *xkey->txt) {
        ctx->set_error(ctx, 400, "cache %s: no <key>", pcache->name);
        return;
    } else {
        cache->key_template = apr_pstrdup(ctx->pool, xkey->txt);
    }

    if (xdebug && xdebug->txt && *xdebug->txt) {
        cache->debug = strcasecmp(xdebug->txt, "true") == 0;
    }
}

/**
 * \private \memberof mapcache_cache_swift
 */
static void _mapcache_cache_swift_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache, mapcache_cfg *cfg) {
}

/**
 * \brief creates and initializes a mapcache_swift_cache
 */
mapcache_cache* mapcache_cache_swift_create(mapcache_context *ctx) {
    mapcache_cache_swift *cache;
    cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_swift));

    if (!cache) {
        ctx->set_error(ctx, 500, "failed to allocate swift cache");
        return NULL;
    }

    cache->cache.metadata = apr_table_make(ctx->pool, 3);
    cache->cache.type = MAPCACHE_CACHE_SWIFT;
    cache->cache._tile_get = _mapcache_cache_swift_get;
    cache->cache._tile_exists = _mapcache_cache_swift_has_tile;
    cache->cache._tile_set = _mapcache_cache_swift_set;
    cache->cache._tile_delete = _mapcache_cache_swift_delete;
    cache->cache.configuration_parse_xml = _mapcache_cache_swift_configuration_parse_xml;
    cache->cache.configuration_post_config = _mapcache_cache_swift_configuration_post_config;
    cache->auth_url = NULL;
    cache->tenant = NULL;
    cache->username = NULL;
    cache->password = NULL;
    cache->key_template = NULL;
    cache->container_template = NULL;

    return (mapcache_cache*)cache;
}

#else
mapcache_cache* mapcache_cache_swift_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"Swift support not compiled in this version");
  return NULL;
}
#endif
