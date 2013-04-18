/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching HTTP request support
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
#include <curl/curl.h>
#include <apr_hash.h>
#include <apr_strings.h>
#include <ctype.h>

#define MAX_STRING_LEN 10000

struct _header_struct {
  apr_table_t *headers;
  mapcache_context *ctx;
};

size_t _mapcache_curl_memory_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  mapcache_buffer *buffer = (mapcache_buffer*)data;
  size_t realsize = size * nmemb;
  return mapcache_buffer_append(buffer, realsize, ptr);
}

size_t _mapcache_curl_header_callback( void *ptr, size_t size, size_t nmemb,  void  *userdata)
{
  char *colonptr;
  struct _header_struct *h = (struct _header_struct*)userdata;
  char *header = apr_pstrndup(h->ctx->pool,ptr,size*nmemb);
  char *endptr = strstr(header,"\r\n");
  if(!endptr) {
    endptr = strstr(header,"\n");
    if(!endptr) {
      /* skip invalid header */
#ifdef DEBUG
      h->ctx->log(h->ctx,MAPCACHE_DEBUG,"received header %s with no trailing \\r\\n",header);
#endif
      return size*nmemb;
    }
  }
  colonptr = strchr(header,':');
  if(colonptr) {
    *colonptr = '\0';
    *endptr = '\0';
    apr_table_setn(h->headers,header,colonptr+2);
  }

  return size*nmemb;
}

void mapcache_http_do_request(mapcache_context *ctx, mapcache_http *req, mapcache_buffer *data, apr_table_t *headers, long *http_code)
{
  CURL *curl_handle;
  char error_msg[CURL_ERROR_SIZE];
  int ret;
  struct curl_slist *curl_headers=NULL;
  curl_handle = curl_easy_init();


  /* specify URL to get */
  curl_easy_setopt(curl_handle, CURLOPT_URL, req->url);
#ifdef DEBUG
  ctx->log(ctx, MAPCACHE_DEBUG, "curl requesting url %s",req->url);
#endif
  /* send all data to this function  */
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, _mapcache_curl_memory_callback);

  /* we pass our mapcache_buffer struct to the callback function */
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)data);

  if(headers != NULL) {
    /* intercept headers */
    struct _header_struct h;
    h.headers = headers;
    h.ctx=ctx;
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, _mapcache_curl_header_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEHEADER, (void*)(&h));
  }

  curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, error_msg);
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, req->connection_timeout);
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, req->timeout);
  curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);



  if(req->headers) {
    const apr_array_header_t *array = apr_table_elts(req->headers);
    apr_table_entry_t *elts = (apr_table_entry_t *) array->elts;
    int i;
    for (i = 0; i < array->nelts; i++) {
      curl_headers = curl_slist_append(curl_headers, apr_pstrcat(ctx->pool,elts[i].key,": ",elts[i].val,NULL));
    }
  }
  if(!req->headers || !apr_table_get(req->headers,"User-Agent")) {
    curl_headers = curl_slist_append(curl_headers, "User-Agent: "MAPCACHE_USERAGENT);
  }
  curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, curl_headers);
  /* get it! */
  ret = curl_easy_perform(curl_handle);
  if(http_code)
    curl_easy_getinfo (curl_handle, CURLINFO_RESPONSE_CODE, http_code);
  else
    curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1);

  if(ret != CURLE_OK) {
    ctx->set_error(ctx, 502, "curl failed to request url %s : %s", req->url, error_msg);
  }
  /* cleanup curl stuff */
  curl_easy_cleanup(curl_handle);
}

void mapcache_http_do_request_with_params(mapcache_context *ctx, mapcache_http *req, apr_table_t *params,
    mapcache_buffer *data, apr_table_t *headers, long *http_code)
{
  mapcache_http *request = mapcache_http_clone(ctx,req);
  request->url = mapcache_http_build_url(ctx,req->url,params);
  mapcache_http_do_request(ctx,request,data,headers, http_code);
}

/* calculate the length of the string formed by key=value&, and add it to cnt */
#ifdef _WIN32
static int _mapcache_key_value_strlen_callback(void *cnt, const char *key, const char *value)
{
#else
static APR_DECLARE_NONSTD(int) _mapcache_key_value_strlen_callback(void *cnt, const char *key, const char *value)
{
#endif
  *((int*)cnt) += strlen(key) + 2 + ((value && *value) ? strlen(value) : 0);
  return 1;
}

#ifdef _WIN32
static int _mapcache_key_value_append_callback(void *cnt, const char *key, const char *value)
{
#else
static APR_DECLARE_NONSTD(int) _mapcache_key_value_append_callback(void *cnt, const char *key, const char *value)
{
#endif
#define _mystr *((char**)cnt)
  _mystr = apr_cpystrn(_mystr,key,MAX_STRING_LEN);
  *((_mystr)++) = '=';
  if(value && *value) {
    _mystr = apr_cpystrn(_mystr,value,MAX_STRING_LEN);
  }
  *((_mystr)++) = '&';
  return 1;
#undef _mystr
}

static char _mapcache_x2c(const char *what)
{
  register char digit;
  digit = ((what[0] >= 'A') ? ((what[0] & 0xdf) - 'A') + 10
           : (what[0] - '0'));
  digit *= 16;
  digit += (what[1] >= 'A' ? ((what[1] & 0xdf) - 'A') + 10
            : (what[1] - '0'));
  return (digit);
}

#ifdef _WIN32
#define IS_SLASH(s) ((s == '/') || (s == '\\'))
#else
#define IS_SLASH(s) (s == '/')
#endif

int _mapcache_unescape_url(char *url)
{
  register int badesc, badpath;
  char *x, *y;

  badesc = 0;
  badpath = 0;
  /* Initial scan for first '%'. Don't bother writing values before
   * seeing a '%' */
  y = strchr(url, '%');
  if (y == NULL) {
    return MAPCACHE_SUCCESS;
  }
  for (x = y; *y; ++x, ++y) {
    if (*y != '%')
      *x = *y;
    else {
      if (!isxdigit(*(y + 1)) || !isxdigit(*(y + 2))) {
        badesc = 1;
        *x = '%';
      } else {
        *x = _mapcache_x2c(y + 1);
        y += 2;
        if (IS_SLASH(*x) || *x == '\0')
          badpath = 1;
      }
    }
  }
  *x = '\0';
  if (badesc)
    return MAPCACHE_FAILURE;
  else if (badpath)
    return MAPCACHE_FAILURE;
  else
    return MAPCACHE_SUCCESS;
}



char* mapcache_http_build_url(mapcache_context *r, char *base, apr_table_t *params)
{
  if(!apr_is_empty_table(params)) {
    int stringLength = 0, baseLength;
    char *builtUrl,*builtUrlPtr;
    char charToAppend=0;
    baseLength = strlen(base);

    /*calculate the length of the param string we are going to build */
    apr_table_do(_mapcache_key_value_strlen_callback, (void*)&stringLength, params, NULL);

    if(strchr(base,'?')) {
      /* base already contains a '?' , shall we be adding a '&' to the end */
      if(base[baseLength-1] != '?' && base[baseLength-1] != '&') {
        charToAppend = '&';
      }
    } else {
      /* base does not contain a '?', we will be adding it */
      charToAppend='?';
    }

    /* add final \0 and eventual separator to add ('?' or '&') */
    stringLength += baseLength + ((charToAppend)?2:1);

    builtUrl = builtUrlPtr = apr_palloc(r->pool, stringLength);

    builtUrlPtr = apr_cpystrn(builtUrlPtr,base,MAX_STRING_LEN);
    if(charToAppend)
      *(builtUrlPtr++)=charToAppend;
    apr_table_do(_mapcache_key_value_append_callback, (void*)&builtUrlPtr, params, NULL);
    *(builtUrlPtr-1) = '\0'; /*replace final '&' by a \0 */
    return builtUrl;
  } else {
    return base;
  }
}

/* Parse form data from a string. The input string is preserved. */
apr_table_t *mapcache_http_parse_param_string(mapcache_context *r, char *args_str)
{
  apr_table_t *params;
  char *args = apr_pstrdup(r->pool,args_str);
  char *key;
  char *value;
  const char *delim = "&";
  char *last;
  if (args == NULL) {
    return apr_table_make(r->pool,0);
  }
  params = apr_table_make(r->pool,20);
  /* Split the input on '&' */
  for (key = apr_strtok(args, delim, &last); key != NULL;
       key = apr_strtok(NULL, delim, &last)) {
    /* key is a pointer to the key=value string */
    /*loop through key=value string to replace '+' by ' ' */
    for (value = key; *value; ++value) {
      if (*value == '+') {
        *value = ' ';
      }
    }

    /* split into Key / Value and unescape it */
    value = strchr(key, '=');
    if (value) {
      *value++ = '\0'; /* replace '=' by \0, thus terminating the key string */
      _mapcache_unescape_url(key);
      _mapcache_unescape_url(value);
    } else {
      value = "";
      _mapcache_unescape_url(key);
    }
    /* Store key/value pair in our form hash. */
    apr_table_addn(params, key, value);
  }
  return params;
}

#ifdef DEBUG
static void http_cleanup(void *dummy)
{
  curl_global_cleanup();
}
#endif

mapcache_http* mapcache_http_configuration_parse_xml(mapcache_context *ctx, ezxml_t node)
{
  ezxml_t http_node;
  mapcache_http *req;
  curl_global_init(CURL_GLOBAL_ALL);
#ifdef DEBUG
  /* make valgrind happy */
  apr_pool_cleanup_register(ctx->pool, NULL,(void*)http_cleanup, apr_pool_cleanup_null);
#endif
  req = (mapcache_http*)apr_pcalloc(ctx->pool,
                                    sizeof(mapcache_http));
  if ((http_node = ezxml_child(node,"url")) != NULL) {
    req->url = apr_pstrdup(ctx->pool,http_node->txt);
  }
  if(!req->url) {
    ctx->set_error(ctx,400,"got an <http> object with no <url>");
    return NULL;
  }

  if ((http_node = ezxml_child(node,"connection_timeout")) != NULL) {
    char *endptr;
    req->connection_timeout = (int)strtol(http_node->txt,&endptr,10);
    if(*endptr != 0 || req->connection_timeout<1) {
      ctx->set_error(ctx,400,"invalid <http> <connection_timeout> \"%s\" (positive integer expected)",
                     http_node->txt);
      return NULL;
    }
  } else {
    req->connection_timeout = 30;
  }
  
  if ((http_node = ezxml_child(node,"timeout")) != NULL) {
    char *endptr;
    req->timeout = (int)strtol(http_node->txt,&endptr,10);
    if(*endptr != 0 || req->timeout<1) {
      ctx->set_error(ctx,400,"invalid <http> <timeout> \"%s\" (positive integer expected)",
                     http_node->txt);
      return NULL;
    }
  } else {
    req->timeout = 600;
  }

  req->headers = apr_table_make(ctx->pool,1);
  if((http_node = ezxml_child(node,"headers")) != NULL) {
    ezxml_t header_node;
    for(header_node = http_node->child; header_node; header_node = header_node->sibling) {
      apr_table_set(req->headers, header_node->name, header_node->txt);
    }
  }
  return req;
  /* TODO: parse <proxy> and <auth> elements */
}


mapcache_http* mapcache_http_clone(mapcache_context *ctx, mapcache_http *orig)
{
  mapcache_http *ret = apr_pcalloc(ctx->pool, sizeof(mapcache_http));
  ret->headers = apr_table_clone(ctx->pool,orig->headers);
  ret->url = apr_pstrdup(ctx->pool, orig->url);
  ret->connection_timeout = orig->connection_timeout;
  ret->timeout = orig->timeout;
  return ret;
}

/* vim: ts=2 sts=2 et sw=2
*/
