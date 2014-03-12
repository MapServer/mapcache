/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: OS-level locking support
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
#include <apr_file_io.h>
#include <apr_strings.h>
#include <apr_time.h>

#define MAPCACHE_LOCKFILE_PREFIX "_gc_lock"

char* lock_filename_for_resource(mapcache_context *ctx, mapcache_locker_disk *ldisk, const char *resource)
{
  char *saferes = apr_pstrdup(ctx->pool,resource);
  char *safeptr = saferes;
  while(*safeptr) {
    if(*safeptr==' ' || *safeptr == '/' || *safeptr == '~' || *safeptr == '.') {
      *safeptr = '#';
    }
    safeptr++;
  }
  return apr_psprintf(ctx->pool,"%s/"MAPCACHE_LOCKFILE_PREFIX"%s.lck",
                      ldisk->dir,saferes);
}

int mapcache_lock_or_wait_for_resource(mapcache_context *ctx, char *resource)
{
  return ctx->config->locker->lock_or_wait(ctx, ctx->config->locker, resource);
}

void mapcache_locker_disk_clear_all_locks(mapcache_context *ctx, mapcache_locker *self) {
  mapcache_locker_disk *ldisk = (mapcache_locker_disk*)self;
  apr_dir_t *lockdir;
  char errmsg[120];
  apr_finfo_t finfo;
  apr_status_t rv = apr_dir_open(&lockdir,ldisk->dir,ctx->pool);
  if(rv != APR_SUCCESS) {
    ctx->set_error(ctx,500, "failed to open lock directory %s: %s" ,ldisk->dir,apr_strerror(rv,errmsg,120));
    return;
  }

  while ((apr_dir_read(&finfo, APR_FINFO_DIRENT|APR_FINFO_TYPE|APR_FINFO_NAME, lockdir)) == APR_SUCCESS) {
    if(finfo.filetype == APR_REG) {
      if(!strncmp(finfo.name, MAPCACHE_LOCKFILE_PREFIX, strlen(MAPCACHE_LOCKFILE_PREFIX))) {
        ctx->log(ctx,MAPCACHE_WARN,"found old lockfile %s/%s, deleting it",ldisk->dir,
            finfo.name);
        rv = apr_file_remove(apr_psprintf(ctx->pool,"%s/%s",ldisk->dir, finfo.name),ctx->pool);
        if(rv != APR_SUCCESS) {
          ctx->set_error(ctx,500, "failed to remove lockfile %s: %s",finfo.name,apr_strerror(rv,errmsg,120));
          return;
        }

      }

    }
  }
  apr_dir_close(lockdir);
}
int mapcache_locker_disk_lock_or_wait(mapcache_context *ctx, mapcache_locker *self, char *resource) {
  char *lockname, errmsg[120];
  mapcache_locker_disk *ldisk;
  assert(self->type == MAPCACHE_LOCKER_DISK);
  ldisk = (mapcache_locker_disk*)self;

  lockname = lock_filename_for_resource(ctx,ldisk,resource);
  apr_file_t *lockfile;
  apr_status_t rv;
  /* create the lockfile */
  rv = apr_file_open(&lockfile,lockname,APR_WRITE|APR_CREATE|APR_EXCL|APR_XTHREAD,APR_OS_DEFAULT,ctx->pool);

  /* if the file already exists, wait for it to disappear */
  /* TODO: check the lock isn't stale (i.e. too old) */
  if( rv != APR_SUCCESS ) {
    if( !APR_STATUS_IS_EEXIST(rv) ) {
      ctx->set_error(ctx, 500, "failed to create lockfile %s: %s", lockname, apr_strerror(rv,errmsg,120));
      return MAPCACHE_FAILURE;
    }
    apr_finfo_t info;
    rv = apr_stat(&info,lockname,0,ctx->pool);
#ifdef DEBUG
    if(!APR_STATUS_IS_ENOENT(rv)) {
      ctx->log(ctx, MAPCACHE_DEBUG, "waiting on resource lock %s", resource);
    }
#endif
    while(!APR_STATUS_IS_ENOENT(rv)) {
      /* sleep for the configured number of micro-seconds (default is 1/100th of a second) */
      apr_sleep(ldisk->retry * 1000000);
      rv = apr_stat(&info,lockname,0,ctx->pool);
    }
    return MAPCACHE_FALSE;
  } else {
    /* we acquired the lock */
    apr_file_close(lockfile);
    return MAPCACHE_TRUE;
  }
}

void mapcache_locker_disk_unlock(mapcache_context *ctx, mapcache_locker *self, char *resource)
{
  mapcache_locker_disk *ld = (mapcache_locker_disk*)self;
  char *lockname = lock_filename_for_resource(ctx,ld,resource);
  apr_file_remove(lockname,ctx->pool);
}

void mapcache_unlock_resource(mapcache_context *ctx, char *resource) {
  ctx->config->locker->unlock(ctx, ctx->config->locker, resource);
}

void mapcache_locker_disk_parse_xml(mapcache_context *ctx, mapcache_cfg *cfg, mapcache_locker *self, ezxml_t doc) {
  mapcache_locker_disk *ldisk = (mapcache_locker_disk*)self;
  ezxml_t node;
  if((node = ezxml_child(doc,"directory")) != NULL) {
    ldisk->dir = apr_pstrdup(ctx->pool, node->txt);
  } else {
    ldisk->dir = apr_pstrdup(ctx->pool,"/tmp");
  }

  if((node = ezxml_child(doc,"retry")) != NULL) {
    char *endptr;
    ldisk->retry = strtod(node->txt,&endptr);
    if(*endptr != 0 || ldisk->retry <= 0) {
      ctx->set_error(ctx, 400, "failed to parse retry seconds \"%s\". Expecting a positive floating point number",
          node->txt);
      return;
    }
  } else {
    /* default retry interval is 1/100th of a second */
    ldisk->retry = 0.01;
  }
  /* TODO one day
  if((node = ezxml_child(doc,"lock_prefix")) != NULL) {
    ldisk->prefix = apr_pstrdup(ctx->pool, node->txt);
  } else {
    ldisk->prefix = apr_pstrdup(ctx->pool,"");
  }
  */
}

mapcache_locker* mapcache_locker_disk_create(mapcache_context *ctx) {
  mapcache_locker_disk *ld = (mapcache_locker_disk*)apr_pcalloc(ctx->pool, sizeof(mapcache_locker_disk));
  mapcache_locker *l = (mapcache_locker*)ld;
  l->type = MAPCACHE_LOCKER_DISK;
  l->clear_all_locks = mapcache_locker_disk_clear_all_locks;
  l->lock_or_wait = mapcache_locker_disk_lock_or_wait;
  l->parse_xml = mapcache_locker_disk_parse_xml;
  l->unlock = mapcache_locker_disk_unlock;
  return l;
}

#ifdef USE_MEMCACHE
void mapcache_locker_memcache_parse_xml(mapcache_context *ctx, mapcache_cfg *cfg, mapcache_locker *self, ezxml_t doc) {
  mapcache_locker_memcache *lm = (mapcache_locker_memcache*)self;
  ezxml_t node,server_node;
  char *endptr;
  for(server_node = ezxml_child(doc,"server"); server_node; server_node = server_node->next) {
    lm->nservers++;
  }
  lm->servers = apr_pcalloc(ctx->pool, lm->nservers * sizeof(mapcache_locker_memcache_server));
  lm->nservers = 0;
  for(server_node = ezxml_child(doc,"server"); server_node; server_node = server_node->next) {
    if((node = ezxml_child(server_node,"host")) != NULL) {
      lm->servers[lm->nservers].host = apr_pstrdup(ctx->pool, node->txt);
    } else {
      ctx->set_error(ctx, 400, "memcache locker: no <host> provided");
      return;
    }

    if((node = ezxml_child(server_node,"port")) != NULL) {
      lm->servers[lm->nservers].port = (unsigned int)strtol(node->txt,&endptr,10);
      if(*endptr != 0 || lm->servers[lm->nservers].port <= 0) {
        ctx->set_error(ctx, 400, "failed to parse memcache locker port \"%s\". Expecting a positive integer",
            node->txt);
        return;
      }
    } else {
      /* default memcached port */
      lm->servers[lm->nservers].port = 11211;
    }
    lm->nservers++;
  }

  
  if((node = ezxml_child(doc,"timeout")) != NULL) {
    lm->timeout = (unsigned int)strtol(node->txt,&endptr,10);
    if(*endptr != 0 || lm->timeout <= 0) {
      ctx->set_error(ctx, 400, "failed to parse memcache locker timeout \"%s\". Expecting a positive integer",
          node->txt);
      return;
    }
  } else {
    /* default: timeout after 10 minutes */
    lm->timeout = 600;
  }

  if((node = ezxml_child(doc,"retry")) != NULL) {
    lm->retry = strtod(node->txt,&endptr);
    if(*endptr != 0 || lm->retry <= 0) {
      ctx->set_error(ctx, 400, "failed to parse memcache locker retry \"%s\". Expecting a positive floating point number",
          node->txt);
      return;
    }
  } else {
    /* default: retry every .3 seconds */
    lm->retry = 0.3;
  }
}

static char* memcache_key_for_resource(mapcache_context *ctx, mapcache_locker_memcache *lm, const char *resource)
{
  char *saferes = apr_pstrdup(ctx->pool,resource);
  char *safeptr = saferes;
  while(*safeptr) {
    if(*safeptr==' ' || *safeptr == '/' || *safeptr == '~' || *safeptr == '.' || 
        *safeptr == '\r' || *safeptr == '\n' || *safeptr == '\t' || *safeptr == '\f' || *safeptr == '\e' || *safeptr == '\a' || *safeptr == '\b') {
      *safeptr = '#';
    }
    safeptr++;
  }
  return apr_psprintf(ctx->pool,MAPCACHE_LOCKFILE_PREFIX"%s.lck",saferes);
}

apr_memcache_t* create_memcache(mapcache_context *ctx, mapcache_locker_memcache *lm) {
  apr_status_t rv;
  apr_memcache_t *memcache;
  char errmsg[120];
  int i;
  if(APR_SUCCESS != apr_memcache_create(ctx->pool, lm->nservers, 0, &memcache)) {
    ctx->set_error(ctx,500,"memcache locker: failed to create memcache backend");
    return NULL;
  }

  for(i=0;i<lm->nservers;i++) {
    apr_memcache_server_t *server;
    rv = apr_memcache_server_create(ctx->pool,lm->servers[i].host,lm->servers[i].port,1,1,1,10000,&server);
    if(APR_SUCCESS != rv) {
      ctx->set_error(ctx,500,"memcache locker: failed to create server %s:%d: %s",lm->servers[i].host,lm->servers[i].port, apr_strerror(rv,errmsg,120));
      return NULL;
    }

    rv = apr_memcache_add_server(memcache,server);
    if(APR_SUCCESS != rv) {
      ctx->set_error(ctx,500,"memcache locker: failed to add server %s:%d: %s",lm->servers[i].host,lm->servers[i].port, apr_strerror(rv,errmsg,120));
      return NULL;
    }
  }
  return memcache;
}

int mapcache_locker_memcache_lock_or_wait(mapcache_context *ctx, mapcache_locker *self, char *resource) {
  apr_status_t rv;
  mapcache_locker_memcache *lm = (mapcache_locker_memcache*)self;
  char errmsg[120];
  char *key = memcache_key_for_resource(ctx, lm, resource);
  apr_memcache_t *memcache = create_memcache(ctx,lm);  
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FAILURE;
  }
  rv = apr_memcache_add(memcache,key,"1",1,lm->timeout,0);
  if( rv == APR_SUCCESS) {
    return MAPCACHE_TRUE;
  } else if ( rv == APR_EEXIST ) {
    apr_pool_t *retry_pool;
    char *one;
    size_t ione;
    apr_pool_create(&retry_pool, ctx->pool);
    rv = APR_SUCCESS;
    while(rv == APR_SUCCESS) {
      /* sleep for the configured number of micro-seconds */
      apr_sleep(lm->retry * 1000000);
      rv = apr_memcache_getp(memcache,retry_pool,key,&one,&ione,NULL);
    }
    apr_pool_destroy(retry_pool);
    return MAPCACHE_FALSE;
  } else {
    ctx->set_error(ctx,500,"failed to lock resource %s to memcache locker: %s",resource, apr_strerror(rv,errmsg,120));
    return MAPCACHE_FAILURE;
  }

}

void mapcache_locker_memcache_unlock(mapcache_context *ctx, mapcache_locker *self, char *resource) {
  apr_status_t rv;
  mapcache_locker_memcache *lm = (mapcache_locker_memcache*)self;
  char errmsg[120];
  char *key = memcache_key_for_resource(ctx, lm, resource);
  apr_memcache_t *memcache = create_memcache(ctx,lm);  
  GC_CHECK_ERROR(ctx);
  
  rv = apr_memcache_delete(memcache,key,0);
  if(rv != APR_SUCCESS && rv!= APR_NOTFOUND) {
    ctx->set_error(ctx,500,"memcache: failed to delete key %s: %s", key, apr_strerror(rv,errmsg,120));
  }

}

mapcache_locker* mapcache_locker_memcache_create(mapcache_context *ctx) {
  mapcache_locker_memcache *lm = (mapcache_locker_memcache*)apr_pcalloc(ctx->pool, sizeof(mapcache_locker_memcache));
  mapcache_locker *l = (mapcache_locker*)lm;
  l->type = MAPCACHE_LOCKER_MEMCACHE;
  l->clear_all_locks = NULL;
  l->lock_or_wait = mapcache_locker_memcache_lock_or_wait;
  l->parse_xml = mapcache_locker_memcache_parse_xml;
  l->unlock = mapcache_locker_memcache_unlock;
  lm->nservers = 0;
  lm->servers = NULL;
  return l;
}

#endif
/* vim: ts=2 sts=2 et sw=2
*/
