/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: HTTP Rest cache backend.
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 2014 Regents of the University of Minnesota.
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
#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <curl/curl.h>
#include <apr_base64.h>
#include <apr_md5.h>
#include <math.h>
#include <apr_file_io.h>

typedef struct mapcache_cache_rest mapcache_cache_rest;
typedef struct mapcache_cache_s3 mapcache_cache_s3;
typedef struct mapcache_cache_azure mapcache_cache_azure;
typedef struct mapcache_cache_google mapcache_cache_google;

typedef enum {
  MAPCACHE_REST_METHOD_GET,
  MAPCACHE_REST_METHOD_HEAD,
  MAPCACHE_REST_METHOD_PUT,
  MAPCACHE_REST_METHOD_POST,
  MAPCACHE_REST_METHOD_DELETE
} mapcache_rest_method;

typedef enum {
  MAPCACHE_REST_PROVIDER_NONE,
  MAPCACHE_REST_PROVIDER_S3,
  MAPCACHE_REST_PROVIDER_AZURE,
  MAPCACHE_REST_PROVIDER_GOOGLE
} mapcache_rest_provider;

void sha256(const unsigned char *message, unsigned int len, unsigned char *digest);
void hmac_sha256(const unsigned char *message, unsigned int message_len,
          const unsigned char *key, unsigned int key_size,
          unsigned char *mac, unsigned mac_size);
void hmac_sha1(const char *message, unsigned int message_len,
          const unsigned char *key, unsigned int key_size,
          void *mac);
void sha_hex_encode(unsigned char *sha, unsigned int sha_size);
char *base64_encode(apr_pool_t *pool, const unsigned char *data, size_t input_length);

typedef struct mapcache_rest_operation mapcache_rest_operation;
struct mapcache_rest_operation {
  apr_table_t *headers;
  mapcache_rest_method method;
  char *tile_url;
  char *header_file;
  void (*add_headers)(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers);
};

typedef struct mapcache_rest_configuration mapcache_rest_configuration;
struct mapcache_rest_configuration {
  apr_table_t *common_headers;
  char *tile_url;
  char *header_file;
  mapcache_rest_operation has_tile;
  mapcache_rest_operation get_tile;
  mapcache_rest_operation set_tile;
  mapcache_rest_operation multi_set_tile;
  mapcache_rest_operation delete_tile;
  void (*add_headers)(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers);
};

/**\class mapcache_cache_rest
 * \brief a mapcache_cache on a 3rd party HTTP Rest API
 * \implements mapcache_cache
 */
struct mapcache_cache_rest {
  mapcache_cache cache;
  mapcache_rest_configuration rest;
  int use_redirects;
  int timeout;
  int connection_timeout;
  int detect_blank;
  mapcache_rest_provider provider;
};

struct mapcache_cache_s3 {
  mapcache_cache_rest cache;
  char *id;
  char *secret;
  char *region;
  char *credentials_file;
};

struct mapcache_cache_azure {
  mapcache_cache_rest cache;
  char *id;
  char *secret;
  char *container;
};

struct mapcache_cache_google {
  mapcache_cache_rest cache;
  char *access;
  char *secret;
};

typedef struct {
  mapcache_buffer *buffer;
  size_t offset;
} buffer_struct;

struct rest_conn_params {
  mapcache_cache_rest *cache;
};

void mapcache_rest_connection_constructor(mapcache_context *ctx, void **conn_, void *params) {
  CURL *curl_handle = curl_easy_init();
  if(!curl_handle) {
    ctx->set_error(ctx,500,"failed to create curl handle");
    *conn_ = NULL;
    return;
  }
  *conn_ = curl_handle;
}

void mapcache_rest_connection_destructor(void *conn_) {
  CURL *curl_handle = (CURL*) conn_;
  curl_easy_cleanup(curl_handle);
}

static mapcache_pooled_connection* _rest_get_connection(mapcache_context *ctx, mapcache_cache_rest *cache, mapcache_tile *tile)
{
  mapcache_pooled_connection *pc;
  struct rest_conn_params params;

  params.cache = cache;

  pc = mapcache_connection_pool_get_connection(ctx,cache->cache.name,mapcache_rest_connection_constructor,
          mapcache_rest_connection_destructor, &params);
  if(!GC_HAS_ERROR(ctx) && pc && pc->connection) {
    CURL *curl_handle = (CURL*)pc->connection;
    curl_easy_reset(curl_handle);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, cache->connection_timeout);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, cache->timeout);
  }

  return pc;
}

static size_t buffer_read_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
  buffer_struct *buffer = (buffer_struct*)stream;
  void *start = ((char*)(buffer->buffer->buf)) + buffer->offset;
  size_t bytes = MAPCACHE_MIN((buffer->buffer->size-buffer->offset),(size * nmemb));
  if(bytes) {
    memcpy(ptr,start,bytes);
    buffer->offset += bytes;
  }
  return bytes;
}

size_t buffer_write_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  mapcache_buffer *buffer = (mapcache_buffer*)data;
  size_t realsize = size * nmemb;
  return mapcache_buffer_append(buffer, realsize, ptr);
}

static struct curl_slist* _set_headers(mapcache_context *ctx, CURL *curl, apr_table_t *headers) {
  if(!headers) {
    return NULL;
  } else {
    struct curl_slist *curl_headers=NULL;
    const apr_array_header_t *array = apr_table_elts(headers);
    apr_table_entry_t *elts = (apr_table_entry_t *) array->elts;
    int i;
    for (i = 0; i < array->nelts; i++) {
      if(strlen(elts[i].val) > 0) {
        curl_headers = curl_slist_append(curl_headers, apr_pstrcat(ctx->pool,elts[i].key,": ",elts[i].val,NULL));
      } else {
        curl_headers = curl_slist_append(curl_headers, apr_pstrcat(ctx->pool,elts[i].key,":",NULL));
      }
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    return curl_headers;
  }
}

static void _put_request(mapcache_context *ctx, CURL *curl, mapcache_buffer *buffer, char *url, apr_table_t *headers) {
  CURLcode res;
  buffer_struct data;
  mapcache_buffer *response;
  struct curl_slist *curl_header_data;

  data.buffer = buffer;
  data.offset = 0;

  response = mapcache_buffer_create(10,ctx->pool);

#if LIBCURL_VERSION_NUM < 0x071700
  /*
   * hack around a bug in curl <= 7.22 where the content-length is added
   * a second time even if ti was present in the manually set headers
   */
  apr_table_unset(headers, "Content-Length");
#endif


  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  /* we want to use our own read function */
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, buffer_read_callback);

  /* enable uploading */
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

  /* HTTP PUT please */
  curl_easy_setopt(curl, CURLOPT_PUT, 1L);

  /* don't use an Expect: 100 Continue header */
  apr_table_set(headers, "Expect", "");
  curl_header_data = _set_headers(ctx, curl, headers);

  /* specify target URL, and note that this URL should include a file
   *        name, not only a directory */
  curl_easy_setopt(curl, CURLOPT_URL, url);

  /* now specify which file to upload */
  curl_easy_setopt(curl, CURLOPT_READDATA, &data);

  /* provide the size of the upload, we specicially typecast the value
   *        to curl_off_t since we must be sure to use the correct data size */
  curl_easy_setopt(curl, CURLOPT_INFILESIZE, buffer->size);

  /* send all data to this function  */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);

  /* we pass our mapcache_buffer struct to the callback function */
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);

  /* Now run off and do what you've been told! */
  res = curl_easy_perform(curl);
  /* Check for errors */
  if(res != CURLE_OK) {
    ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest put: %s",curl_easy_strerror(res));
  } else {
    long http_code;
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    if(http_code != 200 && http_code != 201 && http_code != 204) {
      char *msg = response->buf;
      msg[response->size]=0;
      ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest put with code %ld: %s", http_code, msg);
    }
  }

  curl_slist_free_all(curl_header_data);
}

static int _head_request(mapcache_context *ctx, CURL *curl, char *url, apr_table_t *headers) {

  CURLcode res;
  long http_code;
  struct curl_slist *curl_header_data;

  curl_header_data = _set_headers(ctx, curl, headers);

  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  /* specify target URL, and note that this URL should include a file
   *        name, not only a directory */
  curl_easy_setopt(curl, CURLOPT_URL, url);

  curl_easy_setopt(curl, CURLOPT_NOBODY, 1);

  /* Now run off and do what you've been told! */
  res = curl_easy_perform(curl);
  /* Check for errors */
  if(res != CURLE_OK) {
    ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest head %s",curl_easy_strerror(res));
    http_code = 500;
  } else {
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
  }

  curl_slist_free_all(curl_header_data);

  return (int)http_code;
}

static int _delete_request(mapcache_context *ctx, CURL *curl, char *url, apr_table_t *headers) {

  CURLcode res;
  long http_code;
  struct curl_slist *curl_header_data;

  curl_header_data = _set_headers(ctx, curl, headers);

  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  /* specify target URL, and note that this URL should include a file
   *        name, not only a directory */
  curl_easy_setopt(curl, CURLOPT_URL, url);

  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1);

  /* Now run off and do what you've been told! */
  res = curl_easy_perform(curl);
  /* Check for errors */
  if(res != CURLE_OK) {
    ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest delete %s",curl_easy_strerror(res));
    http_code = 500;
  } else {
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
  }

  curl_slist_free_all(curl_header_data);

  return (int)http_code;
}

static mapcache_buffer* _get_request(mapcache_context *ctx, CURL *curl, char *url, apr_table_t *headers) {

  CURLcode res;
  mapcache_buffer *data = NULL;
  long http_code;
  struct curl_slist *curl_header_data;

  curl_header_data = _set_headers(ctx, curl, headers);

  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  data = mapcache_buffer_create(4000, ctx->pool);

  /* send all data to this function  */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);

  /* we pass our mapcache_buffer struct to the callback function */
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)data);

  /* specify target URL, and note that this URL should include a file
   *        name, not only a directory */
  curl_easy_setopt(curl, CURLOPT_URL, url);

  /* Now run off and do what you've been told! */
  res = curl_easy_perform(curl);
  /* Check for errors */
  if(res != CURLE_OK) {
    ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest get: %s",curl_easy_strerror(res));
    data = NULL;
  } else {
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    /* handle special behavior of s3 */
    if(http_code == 403) {
      char *msg = data->buf;
      while(msg && *msg) {
        if(!strncmp(msg,"NoSuchKey",strlen("NoSuchKey"))) {
          ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest get with code %ld: %s", http_code, msg);
          http_code = 404;
          data = NULL;
          break;
        }
        msg++;
      }
    }
    if(http_code != 200 && http_code != 404) {
      char *msg = data->buf;
      msg[data->size]=0;
      ctx->set_error(ctx, 500, "curl_easy_perform() failed in rest get with code %ld: %s", http_code, msg);
    }
    if(http_code == 404) {
      data = NULL; /* not an error */
    }
  }

  curl_slist_free_all(curl_header_data);

  return data;
}

/**
 * @brief _mapcache_cache_rest_add_headers_from_file populate header table from entries found in file
 * @param ctx
 * @param file the file from which headers should be read
 * @param headers the output table which will be populated
 */
void _mapcache_cache_rest_add_headers_from_file(mapcache_context *ctx, char *file, apr_table_t *headers) {
  apr_status_t rv;
  apr_file_t *f;
  if((rv=apr_file_open(&f, file, APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,APR_OS_DEFAULT,
                       ctx->pool)) == APR_SUCCESS) {
    char line[8096];
    while( (rv = apr_file_gets(line,8096,f))== APR_SUCCESS) {
      char *header_name=line, *header_val=line, *header_endval;
      int found_token = MAPCACHE_FALSE;
      /*search for header delimiter (:)*/
      while(header_val && *header_val) {
        if(*header_val == ':') {
          *header_val = '\0';
          found_token = MAPCACHE_TRUE;
          break;
        }
        header_val++;
      }
      if(!found_token) {
        /* malformed line, silently skip it */
        continue;
      }

      header_val++;

      if(!*header_val) {
        /* malformed/empty line, skip it */
        continue;
      }

      header_endval = header_val;
      while(*header_endval) {
        if(*header_endval == '\r' || *header_endval == '\n') {
          *header_endval = '\0';
          break;
        }
        header_endval++;
      }

      if(!*header_val) {
        /* empty header value, skip it */
        continue;
      }

      apr_table_set(headers, header_name, header_val);
    }
    apr_file_close(f);
  } else {
    ctx->set_error(ctx,500,"rest cache: failed to access header file");
  }
}

apr_table_t* _mapcache_cache_rest_headers(mapcache_context *ctx, mapcache_tile *tile, mapcache_rest_configuration *config,
   mapcache_rest_operation *operation) {
  apr_table_t *ret = apr_table_make(ctx->pool,3);
  const apr_array_header_t *array;

  if(config->common_headers) {
    apr_table_entry_t *elts;
    int i;
    array = apr_table_elts(config->common_headers);
    elts = (apr_table_entry_t *) array->elts;
    for (i = 0; i < array->nelts; i++) {
      apr_table_set(ret, elts[i].key,elts[i].val);
    }
  }
  if(config->header_file) {
    _mapcache_cache_rest_add_headers_from_file(ctx,config->header_file,ret);
    if(GC_HAS_ERROR(ctx)) {
      return NULL;
    }
  }
  if(operation->headers) {
    apr_table_entry_t *elts;
    int i;
    array = apr_table_elts(operation->headers);
    elts = (apr_table_entry_t *) array->elts;
    for (i = 0; i < array->nelts; i++) {
      apr_table_set(ret, elts[i].key,elts[i].val);
    }
  }
  if(operation->header_file) {
    _mapcache_cache_rest_add_headers_from_file(ctx,operation->header_file,ret);
    if(GC_HAS_ERROR(ctx)) {
      return NULL;
    }
  }
  return ret;
}

/* Converts an integer value to its hex character*/
static char to_hex(char code) {
  static char hex[] = "0123456789ABCDEF";
  return hex[code & 15];
}

/* Returns a url-encoded version of str */
static char *url_encode(apr_pool_t *pool, char *str) {
  char *pstr = str, *buf = apr_pcalloc(pool, strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
    if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~' || *pstr=='/')
      *pbuf++ = *pstr;
    else if (*pstr == ' ')
      *pbuf++ = '+';
    else
      *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
    pstr++;
  }
  *pbuf = '\0';
  return buf;
}
/**
 * \brief return url for given tile given a template
 *
 * \param tile the tile to get the key from
 * \param template the template to build the url from
 * \param path pointer to a char* that will contain the url
 * \param r
 * \private \memberof mapcache_cache_rest
 */
static void _mapcache_cache_rest_tile_url(mapcache_context *ctx, mapcache_tile *tile, mapcache_rest_configuration *config,
   mapcache_rest_operation *operation, char **url)
{
  char *slashptr,*path;
  int cnt=0;
  if(operation && operation->tile_url) {
    *url = apr_pstrdup(ctx->pool, operation->tile_url);
  } else {
    *url = apr_pstrdup(ctx->pool, config->tile_url);
  }

  *url = mapcache_util_str_replace(ctx->pool, *url, "{tileset}", tile->tileset->name);
  *url = mapcache_util_str_replace(ctx->pool, *url, "{grid}", tile->grid_link->grid->name);
  *url = mapcache_util_str_replace(ctx->pool, *url, "{ext}",
      tile->tileset->format?tile->tileset->format->extension:"png");
  if(strstr(*url,"{x}"))
    *url = mapcache_util_str_replace(ctx->pool,*url, "{x}",
        apr_psprintf(ctx->pool,"%d",tile->x));
  else
    *url = mapcache_util_str_replace(ctx->pool,*url, "{inv_x}",
        apr_psprintf(ctx->pool,"%d",
          tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1));
  if(strstr(*url,"{y}"))
    *url = mapcache_util_str_replace(ctx->pool,*url, "{y}",
        apr_psprintf(ctx->pool,"%d",tile->y));
  else
    *url = mapcache_util_str_replace(ctx->pool,*url, "{inv_y}",
        apr_psprintf(ctx->pool,"%d",
          tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1));
  if(strstr(*url,"{z}"))
    *url = mapcache_util_str_replace(ctx->pool,*url, "{z}",
        apr_psprintf(ctx->pool,"%d",tile->z));
  else
    *url = mapcache_util_str_replace(ctx->pool,*url, "{inv_z}",
        apr_psprintf(ctx->pool,"%d",
          tile->grid_link->grid->nlevels - tile->z - 1));
  if(tile->dimensions) {
    int i;
    if(strstr(*url,"{dim")) {
      char *dimstring="";
      i = tile->dimensions->nelts;
      while(i--) {
        char *single_dim;
        mapcache_requested_dimension *entry = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
        if(!entry->cached_value) {
          ctx->set_error(ctx,500,"BUG: dimension (%s) not defined",entry->dimension->name);
          return;
        }
        dimstring = apr_pstrcat(ctx->pool,dimstring,"#",entry->dimension->name,"#",entry->cached_value,NULL);
        single_dim = apr_pstrcat(ctx->pool,"{dim:",entry->dimension->name,"}",NULL);
        if(strstr(*url,single_dim)) {
          *url = mapcache_util_str_replace(ctx->pool,*url, single_dim, entry->cached_value);
        }
      }
      *url = mapcache_util_str_replace(ctx->pool,*url, "{dim}", dimstring);
    }
  }
  /* url-encode everything after the host name */

  /* find occurence of third "/" in url */
  slashptr = *url;
  while(*slashptr) {
    if(*slashptr == '/') cnt++;
    if(cnt == 3) break;
    slashptr++;
  }
  if(!*slashptr) {
    ctx->set_error(ctx,500,"invalid rest url provided, expecting http(s)://server/path format");
    return;
  }
  path=slashptr;
  path = url_encode(ctx->pool,path);
  *slashptr=0;


  *url = apr_pstrcat(ctx->pool,*url,path,NULL);
  /*ctx->log(ctx,MAPCACHE_DEBUG,"rest url: %s",*url);*/
}


// Simple comparison function for comparing two HTTP header names that are
// embedded within an HTTP header line, returning true if header1 comes
// before header2 alphabetically, false if not
static int headerle(const char *header1, const char *header2)
{
    while (1) {
        if (*header1 == ':') {
            return (*header2 == ':');
        }
        else if (*header2 == ':') {
            return 0;
        }
        else if (*header2 < *header1) {
            return 0;
        }
        else if (*header2 > *header1) {
            return 1;
        }
        header1++, header2++;
    }
}


// Replace this with merge sort eventually, it's the best stable sort.  But
// since typically the number of elements being sorted is small, it doesn't
// matter that much which sort is used, and gnome sort is the world's simplest
// stable sort.  Added a slight twist to the standard gnome_sort - don't go
// forward +1, go forward to the last highest index considered.  This saves
// all the string comparisons that would be done "going forward", and thus
// only does the necessary string comparisons to move values back into their
// sorted position.
static void header_gnome_sort(char **headers, int size)
{
    int i = 0, last_highest = 0;

    while (i < size) {
        if ((i == 0) || headerle(headers[i - 1], headers[i])) {
            i = ++last_highest;
        }
        else {
            char *tmp = headers[i];
            headers[i] = headers[i - 1];
            headers[--i] = tmp;
        }
    }
}

static const char* my_apr_table_get(apr_table_t *t, char *key) {
  const char *val = apr_table_get(t,key);
  if(!val) val = "";
  return val;
}


static void _mapcache_cache_google_headers_add(mapcache_context *ctx, const char* method, mapcache_cache_rest *rcache, mapcache_tile *tile, char *url, apr_table_t *headers)
{
  char *stringToSign, **aheaders, *resource = url, x_amz_date[64];
  const char *head;
  const apr_array_header_t *ahead;
  apr_table_entry_t *elts;
  mapcache_cache_google *google;
  time_t now;
  struct tm *tnow;
  unsigned char sha[65];
  char b64[150];
  int i,nCanonicalHeaders=0,cnt=0;
  assert(rcache->provider == MAPCACHE_REST_PROVIDER_GOOGLE);
  google = (mapcache_cache_google*)rcache;
  now = time(NULL);
  tnow = gmtime(&now);
  sha[64]=0;

  strftime(x_amz_date, 64 , "%a, %d %b %Y %H:%M:%S GMT", tnow);
  apr_table_set(headers,"x-amz-date",x_amz_date);

  if(!strcmp(method,"PUT")) {
    assert(tile->encoded_data);
    apr_md5(sha,tile->encoded_data->buf,tile->encoded_data->size);
    apr_base64_encode(b64, (char*)sha, 16);
    apr_table_set(headers, "Content-MD5", b64);
  }

  head = my_apr_table_get(headers, "Content-MD5");
  stringToSign=apr_pstrcat(ctx->pool, method, "\n", head, "\n", NULL);
  head = my_apr_table_get(headers, "Content-Type");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);

  /* Date: header, left empty as we are using x-amz-date */
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, "\n", NULL);

  ahead = apr_table_elts(headers);
  aheaders = apr_pcalloc(ctx->pool, ahead->nelts * sizeof(char*));
  elts = (apr_table_entry_t *) ahead->elts;

  for (i = 0; i < ahead->nelts; i++) {
    if(!strncmp(elts[i].key,"x-amz-",6)) {
      char *k = aheaders[nCanonicalHeaders] = apr_pstrdup(ctx->pool, elts[i].key);
      while(*k) {
        *k = tolower(*k);
        k++;
      }
      nCanonicalHeaders++;
    }
  }
  header_gnome_sort(aheaders, nCanonicalHeaders);

  for(i=0; i<nCanonicalHeaders; i++) {
    stringToSign = apr_pstrcat(ctx->pool, stringToSign, aheaders[i],":",apr_table_get(headers,aheaders[i]),"\n",NULL);
  }

  /* find occurence of third "/" in url */
  while(*resource) {
    if(*resource == '/') cnt++;
    if(cnt == 3) break;
    resource++;
  }
  if(!*resource) {
    ctx->set_error(ctx,500,"invalid google url provided");
    return;
  }

  stringToSign = apr_pstrcat(ctx->pool, stringToSign, resource, NULL);

  hmac_sha1(stringToSign, strlen(stringToSign), (unsigned char*)google->secret, strlen(google->secret), sha);

  apr_base64_encode(b64, (char*)sha, 20);


  apr_table_set( headers, "Authorization", apr_pstrcat(ctx->pool,"AWS ", google->access, ":", b64, NULL));
}

static void _mapcache_cache_azure_headers_add(mapcache_context *ctx, const char* method, mapcache_cache_rest *rcache, mapcache_tile *tile, char *url, apr_table_t *headers)
{
  char *stringToSign, **aheaders, *canonical_headers="", *canonical_resource=NULL, *resource = url, x_ms_date[64];
  const char *head;
  const apr_array_header_t *ahead;
  apr_table_entry_t *elts;
  mapcache_cache_azure *azure;
  time_t now;
  struct tm *tnow;
  unsigned char sha[65];
  char *b64sign,*keyub64;
  int i,nCanonicalHeaders=0,cnt=0;
  assert(rcache->provider == MAPCACHE_REST_PROVIDER_AZURE);
  azure = (mapcache_cache_azure*)rcache;
  now = time(NULL);
  tnow = gmtime(&now);
  sha[64]=0;

  strftime(x_ms_date, sizeof(x_ms_date), "%a, %d %b %Y %H:%M:%S GMT", tnow);
  apr_table_set(headers,"x-ms-date",x_ms_date);
  apr_table_set(headers,"x-ms-version","2009-09-19");
  apr_table_set(headers,"x-ms-blob-type","BlockBlob");

  stringToSign = apr_pstrcat(ctx->pool, method, "\n", NULL);
  head = my_apr_table_get(headers, "Content-Encoding");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Content-Language");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Content-Length");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Content-MD5");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Content-Type");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Date");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "If-Modified-Since");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "If-Match");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "If-None-Match");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "If-Unmodified-Since");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);
  head = my_apr_table_get(headers, "Range");
  stringToSign=apr_pstrcat(ctx->pool, stringToSign, head, "\n", NULL);

  ahead = apr_table_elts(headers);
  aheaders = apr_pcalloc(ctx->pool, ahead->nelts * sizeof(char*));
  elts = (apr_table_entry_t *) ahead->elts;

  for (i = 0; i < ahead->nelts; i++) {
	char *k;
    if(strncmp(elts[i].key,"x-ms-",5) || elts[i].key[5]==0) continue;
    k = aheaders[nCanonicalHeaders] = apr_pstrdup(ctx->pool, elts[i].key);
    while(*k) {
      *k = tolower(*k);
      k++;
    }
    nCanonicalHeaders++;
  }
  header_gnome_sort(aheaders, nCanonicalHeaders);

  for(i=0; i<nCanonicalHeaders; i++) {
    canonical_headers = apr_pstrcat(ctx->pool, canonical_headers, aheaders[i],":",apr_table_get(headers,aheaders[i]),"\n",NULL);
  }

  /* find occurence of third "/" in url */
  while(*resource) {
    if(*resource == '/') cnt++;
    if(cnt == 3) break;
    resource++;
  }
  if(!*resource) {
    ctx->set_error(ctx,500,"invalid azure url provided");
    return;
  }

  canonical_resource = apr_pstrcat(ctx->pool, "/", azure->id, resource, NULL);

  stringToSign = apr_pstrcat(ctx->pool, stringToSign, canonical_headers, canonical_resource, NULL);

  keyub64 = (char*)apr_pcalloc(ctx->pool, apr_base64_decode_len(azure->secret));
  apr_base64_decode(keyub64, azure->secret);

  hmac_sha256((unsigned char*)stringToSign, strlen(stringToSign), (unsigned char*)keyub64, strlen(keyub64), sha, 32);

  b64sign = (char*)apr_pcalloc(ctx->pool, apr_base64_encode_len(32));
  apr_base64_encode(b64sign, (char*)sha, 32);


  apr_table_set( headers, "Authorization", apr_pstrcat(ctx->pool,"SharedKey ", azure->id, ":", b64sign, NULL));


}

static void _remove_lineends(char *str) {
  if(str) {
    size_t len = strlen(str);
    while(len>0) {
      if(str[len-1] == '\n' || str[len-1] == '\r') {
        str[len-1] = '\0';
        len--;
      } else {
        break;
      }
    }
  }
}

static void _mapcache_cache_s3_headers_add(mapcache_context *ctx, const char* method, mapcache_cache_rest *rcache, mapcache_tile *tile, char *url, apr_table_t *headers)
{
  unsigned char sha1[65],sha2[65];
  int cnt=0,i;
  time_t now = time(NULL);
  struct tm *tnow = gmtime(&now);
  const apr_array_header_t *ahead;
  char *tosign, *key, *canonical_request, x_amz_date[64], *resource = url, **aheaders, *auth;
  apr_table_entry_t *elts;
  mapcache_cache_s3 *s3;
  char *aws_access_key_id = NULL, *aws_secret_access_key = NULL, *aws_security_token = NULL;

  sha1[64]=sha2[64]=0;
  assert(rcache->provider == MAPCACHE_REST_PROVIDER_S3);
  s3 = (mapcache_cache_s3*)rcache;

  if(s3->credentials_file) {
    apr_status_t rv;
    apr_file_t *f;
    if((rv=apr_file_open(&f, s3->credentials_file,
                       APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,APR_OS_DEFAULT,
                       ctx->pool)) == APR_SUCCESS) {
      char line[1024];
      if( (rv = apr_file_gets(line,1024,f))== APR_SUCCESS) {
        _remove_lineends(line);
        aws_access_key_id = apr_pstrdup(ctx->pool,line);
      }
      if( (rv = apr_file_gets(line,1024,f))== APR_SUCCESS) {
        _remove_lineends(line);
        aws_secret_access_key = apr_pstrdup(ctx->pool,line);
      }
      if( (rv = apr_file_gets(line,1024,f))== APR_SUCCESS) {
        _remove_lineends(line);
        aws_security_token = apr_pstrdup(ctx->pool,line);
      }
      apr_file_close(f);
      if(!aws_access_key_id || !*aws_access_key_id|| !aws_secret_access_key ||!*aws_secret_access_key) {
        ctx->set_error(ctx,500,"failed to read access or secret key from credentials file");
      }
      if(aws_security_token && !*aws_security_token) {
        aws_security_token = NULL;
      }
    } else {
      ctx->set_error(ctx,500,"failed to access S3 credential config");
    }
  } else {
    aws_access_key_id = s3->id;
    aws_secret_access_key = s3->secret;
    aws_security_token = NULL;
  }

  if(!strcmp(method,"PUT")) {
    assert(tile->encoded_data);
    sha256((unsigned char*)tile->encoded_data->buf, tile->encoded_data->size, sha1);
    sha_hex_encode(sha1,32);
  } else {
    /* sha256 hash of empty string */
    memcpy(sha1,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",64);
  }
  apr_table_set(headers,"x-amz-content-sha256", (char*)sha1);
  /* sha1 contains the hash of the payload */

  /* find occurence of third "/" in url */
  while(*resource) {
    if(*resource == '/') cnt++;
    if(cnt == 3) break;
    resource++;
  }
  if(!*resource) {
    ctx->set_error(ctx,500,"invalid s3 url provided");
    return;
  }

  strftime(x_amz_date, sizeof(x_amz_date), "%Y%m%dT%H%M%SZ", tnow);
  apr_table_set(headers, "x-amz-date", x_amz_date);

  if(aws_security_token) {
    apr_table_set(headers, "x-amz-security-token", aws_security_token);
  }

  canonical_request = apr_pstrcat(ctx->pool, method, "\n" ,resource, "\n\n",NULL);

  ahead = apr_table_elts(headers);
  aheaders = apr_pcalloc(ctx->pool, ahead->nelts * sizeof(char*));
  elts = (apr_table_entry_t *) ahead->elts;

  for (i = 0; i < ahead->nelts; i++) {
    char *k = aheaders[i] = apr_pstrdup(ctx->pool, elts[i].key);
    while(*k) {
      *k = tolower(*k);
      k++;
    }
  }

  header_gnome_sort(aheaders, ahead->nelts);
  for(i=0; i<ahead->nelts; i++) {
    canonical_request = apr_pstrcat(ctx->pool, canonical_request, aheaders[i],":",apr_table_get(headers,aheaders[i]),"\n",NULL);
  }
  canonical_request = apr_pstrcat(ctx->pool, canonical_request, "\n", NULL);
  for(i=0; i<ahead->nelts; i++) {
    if(i==ahead->nelts-1) {
      canonical_request = apr_pstrcat(ctx->pool, canonical_request, aheaders[i],NULL);
    } else {
      canonical_request = apr_pstrcat(ctx->pool, canonical_request, aheaders[i],";",NULL);
    }
  }
  canonical_request = apr_pstrcat(ctx->pool, canonical_request, "\n", sha1, NULL);
  //printf("canonical request: %s\n",canonical_request);

  tosign = apr_pstrcat(ctx->pool, "AWS4-HMAC-SHA256\n",x_amz_date,"\n",NULL);
  x_amz_date[8]=0;
  sha256((unsigned char*)canonical_request, strlen(canonical_request), sha1);
  sha_hex_encode(sha1,32);
  tosign = apr_pstrcat(ctx->pool, tosign, x_amz_date, "/", s3->region, "/s3/aws4_request\n", sha1,NULL);
  //printf("key to sign: %s\n",tosign);

  key = apr_pstrcat(ctx->pool, "AWS4", aws_secret_access_key, NULL);
  hmac_sha256((unsigned char*)x_amz_date, 8, (unsigned char*)key, strlen(key), sha1, 32);
  hmac_sha256((unsigned char*)s3->region, strlen(s3->region), sha1, 32, sha2, 32);
  hmac_sha256((unsigned char*)"s3", 2, sha2, 32, sha1, 32);
  hmac_sha256((unsigned char*)"aws4_request", 12, sha1, 32, sha2, 32);
  hmac_sha256((unsigned char*)tosign, strlen(tosign), sha2, 32, sha1, 32);
  sha_hex_encode(sha1,32);


  auth = apr_pstrcat(ctx->pool, "AWS4-HMAC-SHA256 Credential=",aws_access_key_id,"/",x_amz_date,"/",s3->region,"/s3/aws4_request,SignedHeaders=",NULL);

  for(i=0; i<ahead->nelts; i++) {
    if(i==ahead->nelts-1) {
      auth = apr_pstrcat(ctx->pool, auth, aheaders[i],NULL);
    } else {
      auth = apr_pstrcat(ctx->pool, auth, aheaders[i],";",NULL);
    }
  }
  auth = apr_pstrcat(ctx->pool, auth, ",Signature=", sha1, NULL);

  apr_table_set(headers, "Authorization", auth);
}

static void _mapcache_cache_s3_put_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_s3_headers_add(ctx, "PUT", pcache, tile, url, headers);
}
static void _mapcache_cache_s3_get_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_s3_headers_add(ctx, "GET", pcache, tile, url, headers);
}
static void _mapcache_cache_s3_head_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_s3_headers_add(ctx, "HEAD", pcache, tile, url, headers);
}
static void _mapcache_cache_s3_delete_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_s3_headers_add(ctx, "DELETE", pcache, tile, url, headers);
}
static void _mapcache_cache_azure_put_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_azure_headers_add(ctx, "PUT", pcache, tile, url, headers);
}
static void _mapcache_cache_azure_get_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_azure_headers_add(ctx, "GET", pcache, tile, url, headers);
}
static void _mapcache_cache_azure_head_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_azure_headers_add(ctx, "HEAD", pcache, tile, url, headers);
}
static void _mapcache_cache_azure_delete_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_azure_headers_add(ctx, "DELETE", pcache, tile, url, headers);
}
static void _mapcache_cache_google_put_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_google_headers_add(ctx, "PUT", pcache, tile, url, headers);
}
static void _mapcache_cache_google_get_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_google_headers_add(ctx, "GET", pcache, tile, url, headers);
}
static void _mapcache_cache_google_head_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_google_headers_add(ctx, "HEAD", pcache, tile, url, headers);
}
static void _mapcache_cache_google_delete_headers_add(mapcache_context *ctx, mapcache_cache_rest *pcache, mapcache_tile *tile, char *url, apr_table_t *headers) {
  _mapcache_cache_google_headers_add(ctx, "DELETE", pcache, tile, url, headers);
}


static int _mapcache_cache_rest_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_rest *rcache = (mapcache_cache_rest*)pcache;
  char *url;
  apr_table_t *headers;
  int status;
  mapcache_pooled_connection *pc;
  CURL *curl;

  _mapcache_cache_rest_tile_url(ctx, tile, &rcache->rest, &rcache->rest.has_tile, &url);
  headers = _mapcache_cache_rest_headers(ctx, tile, &rcache->rest, &rcache->rest.has_tile);

  if(GC_HAS_ERROR(ctx))
    return MAPCACHE_FAILURE;

  if(rcache->rest.add_headers) {
    rcache->rest.add_headers(ctx,rcache,tile,url,headers);
  }
  if(rcache->rest.has_tile.add_headers) {
    rcache->rest.has_tile.add_headers(ctx,rcache,tile,url,headers);
  }

  pc = _rest_get_connection(ctx, rcache, tile);
  if(GC_HAS_ERROR(ctx))
    return MAPCACHE_FAILURE;

  curl = pc->connection;

  status = _head_request(ctx, curl, url, headers);


  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return MAPCACHE_FAILURE;
  }

  mapcache_connection_pool_release_connection(ctx,pc);

  if( status == 200)
    return MAPCACHE_TRUE;
  else
    return MAPCACHE_FALSE;
}

static void _mapcache_cache_rest_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_rest *rcache = (mapcache_cache_rest*)pcache;
  char *url;
  apr_table_t *headers;
  int status;
  mapcache_pooled_connection *pc;
  CURL *curl;
  _mapcache_cache_rest_tile_url(ctx, tile, &rcache->rest, &rcache->rest.delete_tile, &url);
  headers = _mapcache_cache_rest_headers(ctx, tile, &rcache->rest, &rcache->rest.delete_tile);
  GC_CHECK_ERROR(ctx);

  if(rcache->rest.add_headers) {
    rcache->rest.add_headers(ctx,rcache,tile,url,headers);
  }
  if(rcache->rest.delete_tile.add_headers) {
    rcache->rest.delete_tile.add_headers(ctx,rcache,tile,url,headers);
  }

  pc = _rest_get_connection(ctx, rcache, tile);
  GC_CHECK_ERROR(ctx);

  curl = pc->connection;

  status = _delete_request(ctx, curl, url, headers);
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return;
  }
  mapcache_connection_pool_release_connection(ctx,pc);

  if(status!=200 && status!=202 && status!=204 && status!=404 && status!=410) {
    //ctx->set_error(ctx,500,"rest delete returned code %d", status);
  }
}


/**
 * \brief get file content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored in the file
 * \private \memberof mapcache_cache_rest
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_rest_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  mapcache_cache_rest *rcache = (mapcache_cache_rest*)pcache;
  char *url;
  apr_table_t *headers;
  mapcache_pooled_connection *pc;
  CURL *curl;
  _mapcache_cache_rest_tile_url(ctx, tile, &rcache->rest, &rcache->rest.get_tile, &url);
  if(tile->allow_redirect && rcache->use_redirects) {
    tile->redirect = url;
    return MAPCACHE_SUCCESS;
  }
  headers = _mapcache_cache_rest_headers(ctx, tile, &rcache->rest, &rcache->rest.get_tile);

  if(GC_HAS_ERROR(ctx))
    return MAPCACHE_FAILURE;

  if(rcache->rest.add_headers) {
    rcache->rest.add_headers(ctx,rcache,tile,url,headers);
  }
  if(rcache->rest.get_tile.add_headers) {
    rcache->rest.get_tile.add_headers(ctx,rcache,tile,url,headers);
  }

  pc = _rest_get_connection(ctx, rcache, tile);
  if(GC_HAS_ERROR(ctx))
    return MAPCACHE_FAILURE;

  curl = pc->connection;

  tile->encoded_data = _get_request(ctx, curl, url, headers);

  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return MAPCACHE_FAILURE;
  }
  mapcache_connection_pool_release_connection(ctx,pc);

  if(!tile->encoded_data)
    return MAPCACHE_CACHE_MISS;

  return MAPCACHE_SUCCESS;
}

static void _mapcache_cache_rest_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile) {
  mapcache_cache_rest *rcache = (mapcache_cache_rest*)pcache;
  char *url;
  apr_table_t *headers;
  mapcache_pooled_connection *pc;
  CURL *curl;


  if(rcache->detect_blank) {
    if(tile->nodata) {
      return;
    }
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    if(mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      if(tile->raw_image->data[3] == 0) {
        /* We have a blank (uniform) image who's first pixel is fully transparent, thus the whole image is transparent */
        tile->nodata = 1;
        return;
      }
    }
  }

  _mapcache_cache_rest_tile_url(ctx, tile, &rcache->rest, &rcache->rest.set_tile, &url);
  headers = _mapcache_cache_rest_headers(ctx, tile, &rcache->rest, &rcache->rest.set_tile);
  GC_CHECK_ERROR(ctx);

  if(!tile->encoded_data) {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }

  apr_table_set(headers,"Content-Length",apr_psprintf(ctx->pool,"%lu",tile->encoded_data->size));
  if(tile->tileset->format && tile->tileset->format->mime_type)
    apr_table_set(headers, "Content-Type", tile->tileset->format->mime_type);
  else {
    mapcache_image_format_type imgfmt = mapcache_imageio_header_sniff(ctx,tile->encoded_data);
    if(imgfmt == GC_JPEG) {
      apr_table_set(headers, "Content-Type", "image/jpeg");
    } else if (imgfmt == GC_PNG) {
      apr_table_set(headers, "Content-Type", "image/png");
    }
  }

  if(rcache->rest.add_headers) {
    rcache->rest.add_headers(ctx,rcache,tile,url,headers);
  }
  if(rcache->rest.set_tile.add_headers) {
    rcache->rest.set_tile.add_headers(ctx,rcache,tile,url,headers);
  }

  pc = _rest_get_connection(ctx, rcache, tile);
  GC_CHECK_ERROR(ctx);
  curl = pc->connection;

  _put_request(ctx, curl, tile->encoded_data, url, headers);
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
    return;
  }

  mapcache_connection_pool_release_connection(ctx,pc);
}

static void _mapcache_cache_rest_operation_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_rest_operation *op)
{
  ezxml_t cur_node;
  if ((cur_node = ezxml_child(node,"headers")) != NULL) {
    ezxml_t header_node;
    op->headers = apr_table_make(ctx->pool,3);
    for(header_node = cur_node->child; header_node; header_node = header_node->sibling) {
      apr_table_set(op->headers, header_node->name, header_node->txt);
    }
  }
  if ((cur_node = ezxml_child(node,"header_file")) != NULL) {
    op->header_file = apr_pstrdup(ctx->pool, cur_node->txt);
  }
}

/**
 * \private \memberof mapcache_cache_rest
 */
static void _mapcache_cache_rest_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_rest *dcache = (mapcache_cache_rest*)cache;
  if ((cur_node = ezxml_child(node,"url")) != NULL) {
    dcache->rest.tile_url = apr_pstrdup(ctx->pool,cur_node->txt);
  }
  if ((cur_node = ezxml_child(node,"use_redirects")) != NULL) {
    if(!strcasecmp(cur_node->txt,"true")) {
      dcache->use_redirects = 1;
    }
  }
  if ((cur_node = ezxml_child(node,"connection_timeout")) != NULL) {
    char *endptr;
    dcache->connection_timeout = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0 || dcache->connection_timeout<1) {
      ctx->set_error(ctx,400,"invalid rest cache <connection_timeout> \"%s\" (positive integer expected)",
                     cur_node->txt);
      return;
    }
  } else {
    dcache->connection_timeout = 30;
  }

  if ((cur_node = ezxml_child(node,"timeout")) != NULL) {
    char *endptr;
    dcache->timeout = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0 || dcache->timeout<1) {
      ctx->set_error(ctx,400,"invalid rest cache <timeout> \"%s\" (positive integer expected)",
                     cur_node->txt);
      return;
    }
  } else {
    dcache->timeout = 120;
  }

  dcache->detect_blank = 0;
  if ((cur_node = ezxml_child(node, "detect_blank")) != NULL) {
    if(strcasecmp(cur_node->txt,"false")) {
      dcache->detect_blank = 1;
    }
  }

  if ((cur_node = ezxml_child(node,"headers")) != NULL) {
    ezxml_t header_node;
    dcache->rest.common_headers = apr_table_make(ctx->pool,3);
    for(header_node = cur_node->child; header_node; header_node = header_node->sibling) {
      apr_table_set(dcache->rest.common_headers, header_node->name, header_node->txt);
    }
  }

  if ((cur_node = ezxml_child(node,"header_file")) != NULL) {
    dcache->rest.header_file = apr_pstrdup(ctx->pool, cur_node->txt);
  }


  for(cur_node = ezxml_child(node,"operation"); cur_node; cur_node = cur_node->next) {
    char *type = (char*)ezxml_attr(cur_node,"type");
    if(!type) {
      ctx->set_error(ctx,400,"<operation> with no \"type\" attribute in cache (%s)", cache->name);
      return;
    }
    if(!strcasecmp(type,"put")) {
      _mapcache_cache_rest_operation_parse_xml(ctx,cur_node,cache,&dcache->rest.set_tile);
      GC_CHECK_ERROR(ctx);
    } else if(!strcasecmp(type,"get")) {
      _mapcache_cache_rest_operation_parse_xml(ctx,cur_node,cache,&dcache->rest.get_tile);
      GC_CHECK_ERROR(ctx);
    } else if(!strcasecmp(type,"head")) {
      _mapcache_cache_rest_operation_parse_xml(ctx,cur_node,cache,&dcache->rest.has_tile);
      GC_CHECK_ERROR(ctx);
    } else if(!strcasecmp(type,"delete")) {
      _mapcache_cache_rest_operation_parse_xml(ctx,cur_node,cache,&dcache->rest.delete_tile);
      GC_CHECK_ERROR(ctx);
    } else {
      ctx->set_error(ctx,400,"<operation> with unknown \"type\" (%s) attribute in cache (%s) (expecting put, get, head or delete)", type, cache->name);
      return;
    }
  }

}

static void _mapcache_cache_google_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_google *google = (mapcache_cache_google*)cache;
  _mapcache_cache_rest_configuration_parse_xml(ctx, node, cache, config);
  GC_CHECK_ERROR(ctx);
  if ((cur_node = ezxml_child(node,"access")) != NULL) {
    google->access = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"google cache (%s) is missing required <access> child", cache->name);
    return;
  }
  if ((cur_node = ezxml_child(node,"secret")) != NULL) {
    google->secret = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"google cache (%s) is missing required <secret> child", cache->name);
    return;
  }
}

static void _mapcache_cache_s3_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_s3 *s3 = (mapcache_cache_s3*)cache;
  _mapcache_cache_rest_configuration_parse_xml(ctx, node, cache, config);
  GC_CHECK_ERROR(ctx);
  if ((cur_node = ezxml_child(node,"credentials_file")) != NULL) {
    s3->credentials_file = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    if ((cur_node = ezxml_child(node,"id")) != NULL) {
      s3->id = apr_pstrdup(ctx->pool, cur_node->txt);
    } else if ( getenv("AWS_ACCESS_KEY_ID")) {
      s3->id = apr_pstrdup(ctx->pool,getenv("AWS_ACCESS_KEY_ID"));
    } else {
      ctx->set_error(ctx,400,"s3 cache (%s) is missing required <id> child or AWS_ACCESS_KEY_ID environment", cache->name);
      return;
    }
    if ((cur_node = ezxml_child(node,"secret")) != NULL) {
      s3->secret = apr_pstrdup(ctx->pool, cur_node->txt);
    } else if ( getenv("AWS_SECRET_ACCESS_KEY")) {
      s3->secret = apr_pstrdup(ctx->pool,getenv("AWS_SECRET_ACCESS_KEY"));
    } else {
      ctx->set_error(ctx,400,"s3 cache (%s) is missing required <secret> child or AWS_SECRET_ACCESS_KEY environment", cache->name);
      return;
    }
  }
  if ((cur_node = ezxml_child(node,"region")) != NULL) {
    s3->region = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"s3 cache (%s) is missing required <region> child", cache->name);
    return;
  }
}

static void _mapcache_cache_azure_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_azure *azure = (mapcache_cache_azure*)cache;
  _mapcache_cache_rest_configuration_parse_xml(ctx, node, cache, config);
  GC_CHECK_ERROR(ctx);
  if ((cur_node = ezxml_child(node,"id")) != NULL) {
    azure->id = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"azure cache (%s) is missing required <id> child", cache->name);
    return;
  }
  if ((cur_node = ezxml_child(node,"secret")) != NULL) {
    azure->secret = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"azure cache (%s) is missing required <secret> child", cache->name);
    return;
  }
  if ((cur_node = ezxml_child(node,"container")) != NULL) {
    azure->container = apr_pstrdup(ctx->pool, cur_node->txt);
  } else {
    ctx->set_error(ctx,400,"azure cache (%s) is missing required <container> child", cache->name);
    return;
  }
}

/**
 * \private \memberof mapcache_cache_rest
 */
static void _mapcache_cache_rest_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
  mapcache_cache_rest *dcache = (mapcache_cache_rest*)cache;


  if(!dcache->rest.tile_url) {
    if(!dcache->rest.delete_tile.tile_url) {
      ctx->set_error(ctx,400, "rest cache (%s) has no global <url> and no <url> for delete_tile operation", cache->name);
      return;
    }
    if(!dcache->rest.get_tile.tile_url) {
      ctx->set_error(ctx,400, "rest cache (%s) has no global <url> and no <url> for get_tile operation", cache->name);
      return;
    }
    if(!dcache->rest.set_tile.tile_url) {
      ctx->set_error(ctx,400, "rest cache (%s) has no global <url> and no <url> for set_tile operation", cache->name);
      return;
    }
  }
}

void mapcache_cache_rest_init(mapcache_context *ctx, mapcache_cache_rest *cache) {
  cache->use_redirects = 0;
  cache->rest.get_tile.method = MAPCACHE_REST_METHOD_GET;
  cache->rest.set_tile.method = MAPCACHE_REST_METHOD_PUT;
  cache->rest.delete_tile.method = MAPCACHE_REST_METHOD_DELETE;
  cache->rest.multi_set_tile.method = MAPCACHE_REST_METHOD_PUT;
  cache->rest.has_tile.method = MAPCACHE_REST_METHOD_HEAD;
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_REST;
  cache->cache._tile_delete = _mapcache_cache_rest_delete;
  cache->cache._tile_get = _mapcache_cache_rest_get;
  cache->cache._tile_exists = _mapcache_cache_rest_has_tile;
  cache->cache._tile_set = _mapcache_cache_rest_set;
  cache->cache.configuration_post_config = _mapcache_cache_rest_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_rest_configuration_parse_xml;
}
/**
 * \brief creates and initializes a mapcache_rest_cache
 */
mapcache_cache* mapcache_cache_rest_create(mapcache_context *ctx)
{
  mapcache_cache_rest *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_rest));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate rest cache");
    return NULL;
  }
  mapcache_cache_rest_init(ctx,cache);
  cache->provider = MAPCACHE_REST_PROVIDER_NONE;
  return (mapcache_cache*)cache;
}

/**
 * \brief creates and initializes a mapcache_s3_cache
 */
mapcache_cache* mapcache_cache_s3_create(mapcache_context *ctx)
{
  mapcache_cache_s3 *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_s3));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate s3 cache");
    return NULL;
  }
  mapcache_cache_rest_init(ctx,&cache->cache);
  cache->cache.provider = MAPCACHE_REST_PROVIDER_S3;
  cache->cache.cache.configuration_parse_xml = _mapcache_cache_s3_configuration_parse_xml;
  cache->cache.rest.get_tile.add_headers = _mapcache_cache_s3_get_headers_add;
  cache->cache.rest.has_tile.add_headers = _mapcache_cache_s3_head_headers_add;
  cache->cache.rest.set_tile.add_headers = _mapcache_cache_s3_put_headers_add;
  cache->cache.rest.delete_tile.add_headers = _mapcache_cache_s3_delete_headers_add;
  return (mapcache_cache*)cache;
}

/**
 * \brief creates and initializes a mapcache_azure_cache
 */
mapcache_cache* mapcache_cache_azure_create(mapcache_context *ctx)
{
  mapcache_cache_azure *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_azure));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate azure cache");
    return NULL;
  }
  mapcache_cache_rest_init(ctx,&cache->cache);
  cache->cache.provider = MAPCACHE_REST_PROVIDER_AZURE;
  cache->cache.cache.configuration_parse_xml = _mapcache_cache_azure_configuration_parse_xml;
  cache->cache.rest.get_tile.add_headers = _mapcache_cache_azure_get_headers_add;
  cache->cache.rest.has_tile.add_headers = _mapcache_cache_azure_head_headers_add;
  cache->cache.rest.set_tile.add_headers = _mapcache_cache_azure_put_headers_add;
  cache->cache.rest.delete_tile.add_headers = _mapcache_cache_azure_delete_headers_add;
  return (mapcache_cache*)cache;
}

/**
 * \brief creates and initializes a mapcache_google_cache
 */
mapcache_cache* mapcache_cache_google_create(mapcache_context *ctx)
{
  mapcache_cache_google *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_google));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate google cache");
    return NULL;
  }
  mapcache_cache_rest_init(ctx,&cache->cache);
  cache->cache.provider = MAPCACHE_REST_PROVIDER_GOOGLE;
  cache->cache.cache.configuration_parse_xml = _mapcache_cache_google_configuration_parse_xml;
  cache->cache.rest.get_tile.add_headers = _mapcache_cache_google_get_headers_add;
  cache->cache.rest.has_tile.add_headers = _mapcache_cache_google_head_headers_add;
  cache->cache.rest.set_tile.add_headers = _mapcache_cache_google_put_headers_add;
  cache->cache.rest.delete_tile.add_headers = _mapcache_cache_google_delete_headers_add;
  return (mapcache_cache*)cache;
}



/* vim: ts=2 sts=2 et sw=2
*/
