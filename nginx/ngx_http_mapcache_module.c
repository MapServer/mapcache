#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "../include/mapcache.h"
#include <apr_date.h>
#include <apr_strings.h>
#include <apr_pools.h>


apr_pool_t *process_pool = NULL;
static char *ngx_http_mapcache(ngx_conf_t *cf, ngx_command_t *cmd,
                               void *conf);

static ngx_command_t  ngx_http_mapcache_commands[] = {

  {
    ngx_string("mapcache"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_mapcache,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL
  },

  ngx_null_command
};

typedef struct {
  mapcache_context ctx;
  ngx_http_request_t *r;
} mapcache_ngx_context;

static void ngx_mapcache_context_log(mapcache_context *c, mapcache_log_level level, char *message, ...)
{
  mapcache_ngx_context *ctx = (mapcache_ngx_context*)c;
  va_list args;
  if(!c->config || level >= c->config->loglevel) {
    va_start(args,message);
    ngx_log_error(NGX_LOG_ALERT, ctx->r->connection->log, 0,
                  apr_pvsprintf(c->pool,message,args));
    va_end(args);
  }
}

static mapcache_context* ngx_mapcache_context_clone(mapcache_context *ctx)
{
  mapcache_context *nctx = (mapcache_context*)apr_pcalloc(ctx->pool,
                           sizeof(mapcache_ngx_context));
  mapcache_context_copy(ctx,nctx);
  ((mapcache_ngx_context*)nctx)->r = ((mapcache_ngx_context*)ctx)->r;
  apr_pool_create(&nctx->pool,ctx->pool);
  return nctx;
}


static void *
ngx_http_mapcache_create_conf(ngx_conf_t *cf)
{
  apr_initialize();
  atexit(apr_terminate);
  apr_pool_initialize();
  apr_pool_create(&process_pool,NULL);
  mapcache_context *ctx = apr_pcalloc(process_pool, sizeof(mapcache_ngx_context));
  ctx->pool = process_pool;
  ctx->connection_pool = NULL;
  mapcache_context_init(ctx);
  ctx->log = ngx_mapcache_context_log;
  ctx->clone = ngx_mapcache_context_clone;
  ctx->config = NULL;


  return ctx;
}


static void ngx_http_mapcache_write_response(mapcache_context *ctx, ngx_http_request_t *r,
    mapcache_http_response *response)
{
  if(response->mtime) {
    time_t  if_modified_since;
    if(r->headers_in.if_modified_since) {
      if_modified_since = ngx_http_parse_time(r->headers_in.if_modified_since->value.data,
                                              r->headers_in.if_modified_since->value.len);
      if (if_modified_since != NGX_ERROR) {
        apr_time_t apr_if_m_s;
        apr_time_ansi_put ( &apr_if_m_s, if_modified_since);
        if(apr_if_m_s<response->mtime) {
          r->headers_out.status = NGX_HTTP_NOT_MODIFIED;
          ngx_http_send_header(r);
          return;
        }
      }
    }
    char *datestr;
    datestr = apr_palloc(ctx->pool, APR_RFC822_DATE_LEN);
    apr_rfc822_date(datestr, response->mtime);
    apr_table_setn(response->headers,"Last-Modified",datestr);
  }
  if(response->headers && !apr_is_empty_table(response->headers)) {
    const apr_array_header_t *elts = apr_table_elts(response->headers);
    int i;
    for(i=0; i<elts->nelts; i++) {
      apr_table_entry_t entry = APR_ARRAY_IDX(elts,i,apr_table_entry_t);
      if(!strcasecmp(entry.key,"Content-Type")) {
        r->headers_out.content_type.len = strlen(entry.val);
        r->headers_out.content_type.data = (u_char*)entry.val;
      } else {
        ngx_table_elt_t   *h;
        h = ngx_list_push(&r->headers_out.headers);
        if (h == NULL) {
          return;
        }
        h->key.len = strlen(entry.key) ;
        h->key.data = (u_char*)entry.key ;
        h->value.len = strlen(entry.val) ;
        h->value.data = (u_char*)entry.val ;
        h->hash = 1;
      }
    }
  }
  if(response->data) {
    r->headers_out.content_length_n = response->data->size;
  }
  int rc;
  r->headers_out.status = response->code;
  rc = ngx_http_send_header(r);
  if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
    return;
  }

  if(response->data) {
    ngx_buf_t    *b;
    ngx_chain_t   out;
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "Failed to allocate response buffer.");
      return;
    }

    b->pos = ngx_pcalloc(r->pool,response->data->size);
    memcpy(b->pos,response->data->buf,response->data->size);
    b->last = b->pos + response->data->size;
    b->memory = 1;
    b->last_buf = 1;
    b->flush = 1;
    out.buf = b;
    out.next = NULL;
    ngx_http_output_filter(r, &out);
  }

}


static ngx_http_module_t  ngx_http_mapcache_module_ctx = {
  NULL,                          /* preconfiguration */
  NULL,                          /* postconfiguration */

  NULL,                          /* create main configuration */
  NULL,                          /* init main configuration */

  NULL,                          /* create server configuration */
  NULL,                          /* merge server configuration */

  ngx_http_mapcache_create_conf, /* create location configuration */
  NULL                           /* merge location configuration */
};

static ngx_int_t ngx_mapcache_init_process(ngx_cycle_t *cycle)
{
  apr_initialize();
  atexit(apr_terminate);
  apr_pool_initialize();
  apr_pool_create(&process_pool,NULL);
  return NGX_OK;
}

static void ngx_mapcache_exit_process(ngx_cycle_t *cycle)
{
  apr_pool_destroy(process_pool);
}

ngx_module_t  ngx_http_mapcache_module = {
  NGX_MODULE_V1,
  &ngx_http_mapcache_module_ctx, /* module context */
  ngx_http_mapcache_commands,   /* module directives */
  NGX_HTTP_MODULE,               /* module type */
  NULL,                          /* init master */
  NULL,                          /* init module */
  ngx_mapcache_init_process,/* init process */
  NULL,                          /* init thread */
  NULL,                          /* exit thread */
  ngx_mapcache_exit_process,                          /* exit process */
  ngx_mapcache_exit_process,                          /* exit master */
  NGX_MODULE_V1_PADDING
};

static ngx_str_t  pathinfo_str = ngx_string("path_info");
static ngx_int_t pathinfo_index;
static ngx_str_t  urlprefix_str = ngx_string("url_prefix");
static ngx_int_t urlprefix_index;

static ngx_int_t
ngx_http_mapcache_handler(ngx_http_request_t *r)
{
  int ret = NGX_HTTP_OK;
  if (!(r->method & (NGX_HTTP_GET))) {
    return NGX_HTTP_NOT_ALLOWED;
  }
  mapcache_ngx_context *ngctx = ngx_http_get_module_loc_conf(r, ngx_http_mapcache_module);
  mapcache_context *ctx = (mapcache_context*)ngctx;
  apr_pool_create(&(ctx->pool),process_pool);
  ngctx->r = r;
  mapcache_request *request = NULL;
  mapcache_http_response *http_response;

  ngx_http_variable_value_t      *pathinfovv = ngx_http_get_indexed_variable(r, pathinfo_index);

  char* pathInfo = apr_pstrndup(ctx->pool, (char*)pathinfovv->data, pathinfovv->len);
  char *sparams = apr_pstrndup(ctx->pool, (char*)r->args.data, r->args.len);
  apr_table_t *params = mapcache_http_parse_param_string(ctx, sparams);

  mapcache_service_dispatch_request(ctx,&request,pathInfo,params,ctx->config);
  if(GC_HAS_ERROR(ctx) || !request) {
    ngx_http_mapcache_write_response(ctx,r, mapcache_core_respond_to_error(ctx));
    goto cleanup;
  }

  http_response = NULL;
  if(request->type == MAPCACHE_REQUEST_GET_CAPABILITIES) {
    mapcache_request_get_capabilities *req = (mapcache_request_get_capabilities*)request;
    ngx_http_variable_value_t      *urlprefixvv = ngx_http_get_indexed_variable(r, urlprefix_index);
    char *url = apr_pstrcat(ctx->pool,
                            "http://",
                            apr_pstrndup(ctx->pool, (char*)r->headers_in.host->value.data, r->headers_in.host->value.len),
                            apr_pstrndup(ctx->pool, (char*)urlprefixvv->data, urlprefixvv->len),
                            "/",
                            NULL
                           );
    http_response = mapcache_core_get_capabilities(ctx,request->service,req,url,pathInfo,ctx->config);
  } else if( request->type == MAPCACHE_REQUEST_GET_TILE) {
    mapcache_request_get_tile *req_tile = (mapcache_request_get_tile*)request;
    http_response = mapcache_core_get_tile(ctx,req_tile);
  } else if( request->type == MAPCACHE_REQUEST_GET_MAP) {
    mapcache_request_get_map *req_map = (mapcache_request_get_map*)request;
    http_response = mapcache_core_get_map(ctx,req_map);
#ifdef NGINX_RW
  } else if( request->type == MAPCACHE_REQUEST_PROXY ) {
    mapcache_request_proxy *req_proxy = (mapcache_request_proxy*)request;
    http_response = mapcache_core_proxy_request(ctx, req_proxy);
  } else if( request->type == MAPCACHE_REQUEST_GET_FEATUREINFO) {
    mapcache_request_get_feature_info *req_fi = (mapcache_request_get_feature_info*)request;
    http_response = mapcache_core_get_featureinfo(ctx,req_fi);
#endif
#ifdef DEBUG
  } else {
    ctx->set_error(ctx,500,"###BUG### unknown request type");
#endif
  }
  if(GC_HAS_ERROR(ctx)) {
    //   ngx_http_mapcache_write_response(ctx,r, mapcache_core_respond_to_error(ctx));
    goto cleanup;
  }
#ifdef DEBUG
  if(!http_response) {
    ctx->set_error(ctx,500,"###BUG### NULL response");
    ngx_http_mapcache_write_response(ctx,r, mapcache_core_respond_to_error(ctx));
    goto cleanup;
  }
#endif
  ngx_http_mapcache_write_response(ctx,r,http_response);
cleanup:
  if(GC_HAS_ERROR(ctx))
    ret = ctx->_errcode?ctx->_errcode:500;
  ctx->clear_errors(ctx);
  apr_pool_destroy(ctx->pool);
  return ret;
}


static char *
ngx_http_mapcache(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  mapcache_context *ctx = conf;
  ngx_str_t *value;
  value = cf->args->elts;
  char *conffile = (char*)value[1].data;
  ctx->config = mapcache_configuration_create(ctx->pool);
  mapcache_configuration_parse(ctx,conffile,ctx->config,1);
  if(GC_HAS_ERROR(ctx)) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,ctx->get_error_message(ctx));
    return NGX_CONF_ERROR;
  }
  mapcache_configuration_post_config(ctx, ctx->config);
  if(GC_HAS_ERROR(ctx)) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,ctx->get_error_message(ctx));
    return NGX_CONF_ERROR;
  }
  if(mapcache_config_services_enabled(ctx, ctx->config) <= 0) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "no mapcache <service>s configured/enabled, no point in continuing.");
    return NGX_CONF_ERROR;
  }
  mapcache_cache_child_init(ctx,ctx->cfg,ctx->pool);
  if(GC_HAS_ERROR(ctx)) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,ctx->get_error_message(ctx));
    return NGX_CONF_ERROR;
  }
  mapcache_connection_pool_create(ctx->config, &ctx->connection_pool,ctx->pool);
  ctx->config->non_blocking = 1;

  ngx_http_core_loc_conf_t  *clcf;

  clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
  clcf->handler = ngx_http_mapcache_handler;

  pathinfo_index = ngx_http_get_variable_index(cf, &pathinfo_str);
  if (pathinfo_index == NGX_ERROR) {
    return NGX_CONF_ERROR;
  }
  urlprefix_index = ngx_http_get_variable_index(cf, &urlprefix_str);
  if (urlprefix_index == NGX_ERROR) {
    return NGX_CONF_ERROR;
  }

  return NGX_CONF_OK;
}
