/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching apache module implementation
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

/*
 * Include the core server components.
 */
#include "mod_mapcache-config.h"
#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_protocol.h>
#include <http_request.h>
#include <apr_strings.h>
#include <apr_time.h>
#include <http_log.h>
#include "mapcache.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef AP_NEED_SET_MUTEX_PERMS
#include "unixd.h"
#endif

module AP_MODULE_DECLARE_DATA mapcache_module;

typedef struct mapcache_context_apache mapcache_context_apache;
typedef struct mapcache_context_apache_request mapcache_context_apache_request;
typedef struct mapcache_context_apache_server mapcache_context_apache_server;

struct mapcache_context_apache {
  mapcache_context ctx;
};

struct mapcache_context_apache_server {
  mapcache_context_apache ctx;
  server_rec *server;
};

struct mapcache_context_apache_request {
  mapcache_context_apache ctx;
  request_rec *request;
};

typedef struct mapcache_alias_entry mapcache_alias_entry;

struct mapcache_alias_entry {
  char *endpoint;
  char *configfile;
  mapcache_cfg *cfg;
  mapcache_connection_pool *cp;
};

struct mapcache_server_cfg {
  apr_array_header_t *aliases; /**< list of mapcache configurations aliased to a server uri */
  apr_array_header_t *quickaliases; /**< list of mapcache configurations aliased to a server uri */
};
static int mapcache_alias_matches(const char *uri, const char *alias_fakename);

void apache_context_server_log(mapcache_context *c, mapcache_log_level level, char *message, ...)
{
  mapcache_context_apache_server *ctx = (mapcache_context_apache_server*)c;
  va_list args;
  char *msg;
  int ap_log_level;
  va_start(args,message);
  msg = apr_pvsprintf(c->pool,message,args);
  va_end(args);
  switch(level) {
    case MAPCACHE_DEBUG:
      ap_log_level = APLOG_DEBUG;
      break;
    case MAPCACHE_INFO:
      ap_log_level = APLOG_INFO;
      break;
    case MAPCACHE_NOTICE:
      ap_log_level = APLOG_NOTICE;
      break;
    case MAPCACHE_WARN:
      ap_log_level = APLOG_WARNING;
      break;
    case MAPCACHE_ERROR:
      ap_log_level = APLOG_ERR;
      break;
    case MAPCACHE_CRIT:
      ap_log_level = APLOG_CRIT;
      break;
    case MAPCACHE_ALERT:
      ap_log_level = APLOG_ALERT;
      break;
    case MAPCACHE_EMERG:
      ap_log_level = APLOG_EMERG;
      break;
    default:
      ap_log_level = APLOG_WARNING;
  }
  ap_log_error(APLOG_MARK, ap_log_level, 0, ctx->server,"%s",msg);
}

void apache_context_request_log(mapcache_context *c, mapcache_log_level level, char *message, ...)
{
  mapcache_context_apache_request *ctx = (mapcache_context_apache_request*)c;
  va_list args;
  char *res;
  int ap_log_level;
  va_start(args,message);
  res = apr_pvsprintf(c->pool, message, args);
  va_end(args);
  switch(level) {
    case MAPCACHE_DEBUG:
      ap_log_level = APLOG_DEBUG;
      break;
    case MAPCACHE_INFO:
      ap_log_level = APLOG_INFO;
      break;
    case MAPCACHE_NOTICE:
      ap_log_level = APLOG_NOTICE;
      break;
    case MAPCACHE_WARN:
      ap_log_level = APLOG_WARNING;
      break;
    case MAPCACHE_ERROR:
      ap_log_level = APLOG_ERR;
      break;
    case MAPCACHE_CRIT:
      ap_log_level = APLOG_CRIT;
      break;
    case MAPCACHE_ALERT:
      ap_log_level = APLOG_ALERT;
      break;
    case MAPCACHE_EMERG:
      ap_log_level = APLOG_EMERG;
      break;
    default:
      ap_log_level = APLOG_WARNING;
  }
  ap_log_rerror(APLOG_MARK, ap_log_level, 0, ctx->request, "%s", res);
}

mapcache_context *mapcache_context_request_clone(mapcache_context *ctx)
{
  mapcache_context_apache_request *newctx = (mapcache_context_apache_request*)apr_pcalloc(ctx->pool,
      sizeof(mapcache_context_apache_request));
  mapcache_context *nctx = (mapcache_context*)newctx;
  mapcache_context_copy(ctx,nctx);
  //apr_pool_create(&nctx->pool,ctx->pool);
  apr_pool_create(&nctx->pool,NULL);
  apr_pool_cleanup_register(ctx->pool, nctx->pool,(void*)apr_pool_destroy, apr_pool_cleanup_null);
  newctx->request = ((mapcache_context_apache_request*)ctx)->request;
  return nctx;
}

mapcache_context_apache_request* create_apache_request_context(request_rec *r)
{
  mapcache_context_apache_request *rctx = apr_pcalloc(r->pool, sizeof(mapcache_context_apache_request));
  mapcache_context *ctx = (mapcache_context*)rctx;
  mapcache_context_init(ctx);
  ctx->pool = r->pool;
  rctx->request = r;
  ctx->log = apache_context_request_log;
  ctx->clone = mapcache_context_request_clone;
  return rctx;
}

static mapcache_context_apache_server* create_apache_server_context(server_rec *s, apr_pool_t *pool)
{
  mapcache_context_apache_server *actx = apr_pcalloc(pool, sizeof(mapcache_context_apache_server));
  mapcache_context *ctx = (mapcache_context*)actx;
  mapcache_context_init(ctx);
  ctx->pool = pool;
  ctx->config = NULL;
  ctx->log = apache_context_server_log;
  actx->server = s;
  return actx;
}

/* read post body. code taken from "The apache modules book, Nick Kew" */
static void read_post_body(mapcache_context_apache_request *ctx, mapcache_request_proxy *p) {
  request_rec *r = ctx->request;
  mapcache_context *mctx = (mapcache_context*)ctx;
  int bytes,eos;
  apr_bucket_brigade *bb, *bbin;
  apr_bucket *b;
  apr_status_t rv;
  const char *clen = apr_table_get(r->headers_in, "Content-Length");
  if(clen) {
    bytes = strtol(clen, NULL, 0);
    if(bytes >= p->rule->max_post_len) {
      mctx->set_error(mctx, HTTP_REQUEST_ENTITY_TOO_LARGE, "post request too big");
      return;
    }
  } else {
    bytes = p->rule->max_post_len;
  }

  bb = apr_brigade_create(mctx->pool, r->connection->bucket_alloc);
  bbin = apr_brigade_create(mctx->pool, r->connection->bucket_alloc);
  p->post_len = 0;

  do {
    apr_bucket *nextb;
    rv = ap_get_brigade(r->input_filters, bbin, AP_MODE_READBYTES, APR_BLOCK_READ, bytes);
    if(rv != APR_SUCCESS) {
      mctx->set_error(mctx, 500, "failed to read form input");
      return;
    }
    for(b = APR_BRIGADE_FIRST(bbin); b != APR_BRIGADE_SENTINEL(bbin); b = nextb) {
      nextb = APR_BUCKET_NEXT(b);
      if(APR_BUCKET_IS_EOS(b)) {
        eos = 1;
      }
      if(!APR_BUCKET_IS_METADATA(b)) {
        if(b->length != (apr_size_t)(-1)) {
          p->post_len += b->length;
          if(p->post_len > p->rule->max_post_len) {
            apr_bucket_delete(b);
          }
        }
      }
      if(p->post_len <= p->rule->max_post_len) {
        APR_BUCKET_REMOVE(b);
        APR_BRIGADE_INSERT_TAIL(bb, b);
      }
    }
  } while (!eos);

  if(p->post_len > p->rule->max_post_len) {
    mctx->set_error(mctx, HTTP_REQUEST_ENTITY_TOO_LARGE, "request too big");
    return;
  }

  p->post_buf = apr_palloc(mctx->pool, p->post_len+1);
  
  if(p->post_len> 0) {
    rv = apr_brigade_flatten(bb, p->post_buf, &(p->post_len));
    if(rv != APR_SUCCESS) {
      mctx->set_error(mctx, 500, "error (flatten) reading form data");
      return;
    }
  }
  p->post_buf[p->post_len] = 0;
}

static int write_http_response(mapcache_context_apache_request *ctx, mapcache_http_response *response)
{
  request_rec *r = ctx->request;
  int rc;
  char *timestr;

  if(response->mtime) {
    ap_update_mtime(r, response->mtime);
    if((rc = ap_meets_conditions(r)) != OK) {
      return rc;
    }
    timestr = apr_palloc(r->pool, APR_RFC822_DATE_LEN);
    apr_rfc822_date(timestr, response->mtime);
    apr_table_setn(r->headers_out, "Last-Modified", timestr);
  }
  if(response->headers && !apr_is_empty_table(response->headers)) {
    const apr_array_header_t *elts = apr_table_elts(response->headers);
    int i;
    for(i=0; i<elts->nelts; i++) {
      apr_table_entry_t entry = APR_ARRAY_IDX(elts,i,apr_table_entry_t);
      if(!strcasecmp(entry.key,"Content-Type")) {
        ap_set_content_type(r,entry.val);
      } else {
        apr_table_set(r->headers_out, entry.key, entry.val);
      }
    }
  }
  if(response->data && response->data->size) {
    ap_set_content_length(r,response->data->size);
    ap_rwrite((void*)response->data->buf, response->data->size, r);
  }

  r->status = response->code;
  return OK;

}

static void mod_mapcache_child_init(apr_pool_t *pool, server_rec *s)
{
  for( ; s ; s=s->next) {
    mapcache_server_cfg* cfg = ap_get_module_config(s->module_config, &mapcache_module);
    int i,rv;
    for(i=0;i<cfg->aliases->nelts;i++) {
      mapcache_alias_entry *alias_entry = APR_ARRAY_IDX(cfg->aliases,i,mapcache_alias_entry*);
      rv = mapcache_connection_pool_create(&(alias_entry->cp),pool);
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "creating a child process mapcache connection pool on server %s for alias %s", s->server_hostname, alias_entry->endpoint);
      if(rv!=APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "failed to create mapcache connection pool");
      }
    }
    for(i=0;i<cfg->quickaliases->nelts;i++) {
      mapcache_alias_entry *alias_entry = APR_ARRAY_IDX(cfg->quickaliases,i,mapcache_alias_entry*);
      rv = mapcache_connection_pool_create(&(alias_entry->cp),pool);
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "creating a child process mapcache connection pool on server %s for alias %s", s->server_hostname, alias_entry->endpoint);
      if(rv!=APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "failed to create mapcache connection pool");
      }
    }
  }
}

static int mapcache_handler(request_rec *r, mapcache_alias_entry *alias_entry) {
  apr_table_t *params;
  mapcache_request *request = NULL;
  mapcache_context_apache_request *apache_ctx = create_apache_request_context(r);
  mapcache_context *ctx = (mapcache_context*)apache_ctx;
  mapcache_http_response *http_response = NULL;

  ctx->config = alias_entry->cfg;
  ctx->connection_pool = alias_entry->cp;
  ctx->supports_redirects = 1;
  ctx->headers_in = r->headers_in;

  params = mapcache_http_parse_param_string(ctx, r->args);

  //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "mapcache dispatch %s",r->path_info);

  mapcache_service_dispatch_request(ctx,&request,r->path_info,params,ctx->config);
  if(GC_HAS_ERROR(ctx) || !request) {
    return write_http_response(apache_ctx,
                               mapcache_core_respond_to_error(ctx));
  }

  if(request->type == MAPCACHE_REQUEST_GET_CAPABILITIES) {
    mapcache_request_get_capabilities *req_caps = (mapcache_request_get_capabilities*)request;
    request_rec *original;
    char *url;
    if(r->main)
      original = r->main;
    else
      original = r;
    url = ap_construct_url(r->pool,original->uri,original);

    /*
     * remove the path_info from the end of the url (we want the url of the base of the service)
     * TODO: is there an apache api to access this ?
     */
    if(*(original->path_info) && strcmp(original->path_info,"/")) {
      char *end = strstr(url,original->path_info);
      if(end) {
        /* make sure our url ends with a single '/' */
        if(*end == '/') {
          char *slash = end;
          while((*(--slash))=='/') end--;
          end++;
        }
        *end = '\0';
      }
    }
    http_response = mapcache_core_get_capabilities(ctx,request->service,req_caps,
                                                   url,original->path_info,ctx->config);
  } else if( request->type == MAPCACHE_REQUEST_GET_TILE) {
    mapcache_request_get_tile *req_tile = (mapcache_request_get_tile*)request;
    http_response = mapcache_core_get_tile(ctx,req_tile);
  } else if( request->type == MAPCACHE_REQUEST_PROXY ) {
    const char *buf;
    mapcache_request_proxy *req_proxy = (mapcache_request_proxy*)request;
    if(r->method_number == M_POST) {
      read_post_body(apache_ctx, req_proxy);
      if(GC_HAS_ERROR(ctx)) {
        return write_http_response(apache_ctx, mapcache_core_respond_to_error(ctx));
      }
      if(!req_proxy->headers) {
        req_proxy->headers = apr_table_make(ctx->pool, 2);
      }
      apr_table_set(req_proxy->headers, "Content-Type", r->content_type);
      if((buf = apr_table_get(r->headers_in,"X-Forwarded-For"))) {
#if (AP_SERVER_MAJORVERSION_NUMBER == 2) && (AP_SERVER_MINORVERSION_NUMBER < 4)
        apr_table_set(req_proxy->headers, "X-Forwarded-For", apr_psprintf(ctx->pool,"%s, %s", buf, r->connection->remote_ip));
#else
        apr_table_set(req_proxy->headers, "X-Forwarded-For", apr_psprintf(ctx->pool,"%s, %s", buf, r->connection->client_ip));
#endif
      } else {
#if (AP_SERVER_MAJORVERSION_NUMBER == 2) && (AP_SERVER_MINORVERSION_NUMBER < 4)
        apr_table_set(req_proxy->headers, "X-Forwarded-For", r->connection->remote_ip);
#else
        apr_table_set(req_proxy->headers, "X-Forwarded-For", r->connection->client_ip);
#endif
      }
      if ((buf = apr_table_get(r->headers_in, "Host"))) {
        const char *buf2;
        if((buf2 = apr_table_get(r->headers_in,"X-Forwarded-Host"))) {
          apr_table_set(req_proxy->headers, "X-Forwarded-Host", apr_psprintf(ctx->pool,"%s, %s",buf2,buf));
        } else {
          apr_table_set(req_proxy->headers, "X-Forwarded-Host", buf);
        }
      }

      if ((buf = apr_table_get(r->headers_in, "X-Forwarded-Server"))) {
        apr_table_set(req_proxy->headers, "X-Forwarded-Server", apr_psprintf(ctx->pool, "%s, %s", buf, r->server->server_hostname));
      } else {
        apr_table_set(req_proxy->headers, "X-Forwarded-Server", r->server->server_hostname);
      }
    }
    http_response = mapcache_core_proxy_request(ctx, req_proxy);
  } else if( request->type == MAPCACHE_REQUEST_GET_MAP) {
    mapcache_request_get_map *req_map = (mapcache_request_get_map*)request;
    http_response = mapcache_core_get_map(ctx,req_map);
  } else if( request->type == MAPCACHE_REQUEST_GET_FEATUREINFO) {
    mapcache_request_get_feature_info *req_fi = (mapcache_request_get_feature_info*)request;
    http_response = mapcache_core_get_featureinfo(ctx,req_fi);
  } else {
    ctx->set_error(ctx,500,"###BUG### unknown request type");
  }

  if(GC_HAS_ERROR(ctx)) {
    return write_http_response(apache_ctx,
                               mapcache_core_respond_to_error(ctx));
  }
  return write_http_response(apache_ctx,http_response);
}

static int mod_mapcache_quick_handler(request_rec *r, int lookup) {
  mapcache_server_cfg *sconfig = ap_get_module_config(r->server->module_config, &mapcache_module);
  mapcache_alias_entry *alias_entry;
  int i;

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "mapcache quick handler hook on uri %s",r->uri);

  if (!sconfig || !sconfig->quickaliases)
    return DECLINED;

  if (r->uri[0] != '/' && r->uri[0])
    return DECLINED;
  
  if(lookup) {
    return DECLINED;
  }

  /* loop through the entries to find one where the alias matches */
  for(i=0; i<sconfig->quickaliases->nelts; i++) {
    int l;
    alias_entry = APR_ARRAY_IDX(sconfig->quickaliases,i,mapcache_alias_entry*);
    //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "cheking mapcache alias %s against %s",r->uri,alias_entry->endpoint);

    if((l=mapcache_alias_matches(r->uri, alias_entry->endpoint))>0) {
      if (r->method_number != M_GET && r->method_number != M_POST) {
        return HTTP_METHOD_NOT_ALLOWED;
      }
      r->path_info = &(r->uri[l]);
      return mapcache_handler(r,alias_entry);
    }
  }
  return DECLINED;
}

static int mod_mapcache_request_handler(request_rec *r)
{
  const char *mapcache_alias;
  int i;
  mapcache_server_cfg* cfg;
  if (!r->handler || strcmp(r->handler, "mapcache")) {
    return DECLINED;
  }
  if (r->method_number != M_GET && r->method_number != M_POST) {
    return HTTP_METHOD_NOT_ALLOWED;
  }
  cfg = ap_get_module_config(r->server->module_config, &mapcache_module);
  mapcache_alias = apr_table_get(r->notes,"mapcache_alias_entry");
  //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "using mapcache config %s", mapcache_config_file);
  if(!mapcache_alias) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "mapcache module bug? no mapcache_alias_entry found");
    return DECLINED;
  }

  for(i=0; i<cfg->aliases->nelts; i++) {
    mapcache_alias_entry *alias_entry = APR_ARRAY_IDX(cfg->aliases,i,mapcache_alias_entry*);
    if(strcmp(alias_entry->endpoint,mapcache_alias))
      continue;
    return mapcache_handler(r,alias_entry);
  }
  return DECLINED; /*should never happen, the fixup phase would not have oriented us here*/
}

static int mod_mapcache_post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
  mapcache_server_cfg* cfg = ap_get_module_config(s->module_config, &mapcache_module);
  if(!cfg) {
    ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "configuration not found in server context");
    return 1;
  }

#ifdef USE_VERSION_STRING
  ap_add_version_component(p, MAPCACHE_USERAGENT);
#endif

  return 0;
}

static int mapcache_alias_matches(const char *uri, const char *alias_fakename)
{
  /* Code for this function from Apache mod_alias module. */

  const char *aliasp = alias_fakename, *urip = uri;

  while (*aliasp) {
    if (*aliasp == '/') {
      /* any number of '/' in the alias matches any number in
       * the supplied URI, but there must be at least one...
       */
      if (*urip != '/')
        return 0;

      do {
        ++aliasp;
      } while (*aliasp == '/');
      do {
        ++urip;
      } while (*urip == '/');
    } else {
      /* Other characters are compared literally */
      if (*urip++ != *aliasp++)
        return 0;
    }
  }

  /* Check last alias path component matched all the way */

  if (aliasp[-1] != '/' && *urip != '\0' && *urip != '/')
    return 0;

  /* Return number of characters from URI which matched (may be
   * greater than length of alias, since we may have matched
   * doubled slashes)
   */
  return urip - uri;
}

static int mapcache_hook_fixups(request_rec *r)
{
  int i;
  //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "running mapcache fixup on %s (handler:%s,filename:%s)",r->uri,r->handler,r->filename);
  if(!r->handler) {
    mapcache_server_cfg *sconfig = ap_get_module_config(r->server->module_config, &mapcache_module);
    mapcache_alias_entry *alias_entry;

    if (!sconfig || !sconfig->aliases)
      return DECLINED;

    if (r->uri[0] != '/' && r->uri[0])
      return DECLINED;

    /* loop through the entries to find one where the alias matches */
    for(i=0; i<sconfig->aliases->nelts; i++) {
      int l;
      alias_entry = APR_ARRAY_IDX(sconfig->aliases,i,mapcache_alias_entry*);
      //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "cheking mapcache alias %s against %s",r->uri,alias_entry->endpoint);

      if((l=mapcache_alias_matches(r->uri, alias_entry->endpoint))>0) {
        r->handler = apr_pstrdup(r->pool,"mapcache");
        apr_table_set(r->notes,"mapcache_alias_entry",alias_entry->endpoint);
        r->path_info = &(r->uri[l]);
        //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "setting config %s for alias %s",alias_entry->configfile,alias_entry->endpoint);
        //ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "using pathinfo %s from uri %s",r->path_info,r->uri);
        return OK;
      }
    }
  }

  return DECLINED;
}


static void mod_mapcache_register_hooks(apr_pool_t *p)
{
  ap_hook_child_init(mod_mapcache_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(mod_mapcache_post_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_handler(mod_mapcache_request_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_quick_handler(mod_mapcache_quick_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_fixups(mapcache_hook_fixups, NULL, NULL, APR_HOOK_MIDDLE);

}

static void* mod_mapcache_create_server_conf(apr_pool_t *pool, server_rec *s)
{
  mapcache_server_cfg *cfg = apr_pcalloc(pool, sizeof(mapcache_server_cfg));
  cfg->aliases = apr_array_make(pool,1,sizeof(mapcache_alias_entry*));
  cfg->quickaliases = apr_array_make(pool,1,sizeof(mapcache_alias_entry*));
  return cfg;
}


static void *mod_mapcache_merge_server_conf(apr_pool_t *p, void *base_, void *vhost_)
{
  mapcache_server_cfg *base = (mapcache_server_cfg*)base_;
  mapcache_server_cfg *vhost = (mapcache_server_cfg*)vhost_;
  mapcache_server_cfg *cfg = apr_pcalloc(p,sizeof(mapcache_server_cfg));

  cfg->aliases = apr_array_append(p, vhost->aliases,base->aliases);
  cfg->quickaliases = apr_array_append(p, vhost->quickaliases,base->quickaliases);

#if 0
  {
    mapcache_alias_entry *e;
    int i;
    fprintf(stderr,"#### merge called ####\n");
    for(i=0;i<base->aliases->nelts;i++) {
      e = APR_ARRAY_IDX(base->aliases,i,mapcache_alias_entry*);
      fprintf(stderr,"merge base: have alias %s on %s\n",e->configfile,e->endpoint);
    }
    for(i=0;i<vhost->aliases->nelts;i++) {
      e = APR_ARRAY_IDX(vhost->aliases,i,mapcache_alias_entry*);
      fprintf(stderr,"merge vhosts: have alias %s on %s\n",e->configfile,e->endpoint);
    }
    for(i=0;i<cfg->aliases->nelts;i++) {
      e = APR_ARRAY_IDX(cfg->aliases,i,mapcache_alias_entry*);
      fprintf(stderr,"merge result: have alias %s on %s\n",e->configfile,e->endpoint);
    }
  }
#endif
  return cfg;
}

static const char* mapcache_add_alias(cmd_parms *cmd, void *cfg, const char *alias, const char* configfile, const char *quick)
{
  mapcache_server_cfg *sconfig;
  mapcache_alias_entry *alias_entry;
  mapcache_context *ctx;
  unsigned forbidden = NOT_IN_DIRECTORY|NOT_IN_FILES;
  const char *err;

#if (AP_SERVER_MAJORVERSION_NUMBER > 2) || (AP_SERVER_MINORVERSION_NUMBER >= 4)
  forbidden |= NOT_IN_HTACCESS;
#endif

  err = ap_check_cmd_context(cmd, forbidden);
  if (err) {
    return err;
  }

  sconfig = ap_get_module_config(cmd->server->module_config, &mapcache_module);
  if(!sconfig || !sconfig->aliases)
    return "no mapcache module config, server bug?";

  alias_entry = apr_pcalloc(cmd->pool,sizeof(mapcache_alias_entry));
  ctx = (mapcache_context*)create_apache_server_context(cmd->server,cmd->pool);

  alias_entry->cfg = mapcache_configuration_create(cmd->pool);
  alias_entry->configfile = apr_pstrdup(cmd->pool,configfile);
  alias_entry->endpoint = apr_pstrdup(cmd->pool,alias);
  mapcache_configuration_parse(ctx,alias_entry->configfile,alias_entry->cfg,0);
  if(GC_HAS_ERROR(ctx)) {
    return ctx->get_error_message(ctx);
  }
  mapcache_configuration_post_config(ctx, alias_entry->cfg);
  if(GC_HAS_ERROR(ctx)) {
    return ctx->get_error_message(ctx);
  }
  if(mapcache_config_services_enabled(ctx, alias_entry->cfg) <= 0) {
    return "no mapcache <service>s configured/enabled, no point in continuing.";
  }
  if(quick && !strcmp(quick,"quick")) {
    APR_ARRAY_PUSH(sconfig->quickaliases,mapcache_alias_entry*) = alias_entry;
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, cmd->server, "loaded mapcache configuration file from %s on (quick) endpoint %s", alias_entry->configfile, alias_entry->endpoint);
  } else {
    APR_ARRAY_PUSH(sconfig->aliases,mapcache_alias_entry*) = alias_entry;
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, cmd->server, "loaded mapcache configuration file from %s on endpoint %s", alias_entry->configfile, alias_entry->endpoint);
  }

  return NULL;
}



static const command_rec mod_mapcache_cmds[] = {
  AP_INIT_TAKE23("MapCacheAlias", mapcache_add_alias ,NULL,RSRC_CONF,"Aliased location of configuration file"),
  { NULL }
} ;

module AP_MODULE_DECLARE_DATA mapcache_module = {
  STANDARD20_MODULE_STUFF,
  NULL,
  NULL,
  mod_mapcache_create_server_conf,
  mod_mapcache_merge_server_conf,
  mod_mapcache_cmds,
  mod_mapcache_register_hooks
};
/* vim: ts=2 sts=2 et sw=2
*/
