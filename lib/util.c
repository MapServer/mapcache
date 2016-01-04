/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: common utility functions
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
#include "util.h"
#include <apr_strings.h>
#include <apr_tables.h>
#include <curl/curl.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327
#endif

#ifdef _WIN32
typedef unsigned char     uint8_t;
typedef unsigned short    uint16_t;
typedef unsigned int      uint32_t;
typedef unsigned long int uint64_t;
#endif

const double mapcache_meters_per_unit[MAPCACHE_UNITS_COUNT] = {1.0,6378137.0 * 2.0 * M_PI / 360,0.3048};


static char encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                '4', '5', '6', '7', '8', '9', '+', '/'};
static int mod_table[] = {0, 2, 1};


char *base64_encode(apr_pool_t *pool, const unsigned char *data, size_t input_length) {
  int i,j;
  char *encoded_data;
  size_t output_length = 4 * ((input_length + 2) / 3) + 1;

  encoded_data = (char*)apr_pcalloc(pool,output_length*sizeof(char));
  if (encoded_data == NULL) return NULL;

  for (i = 0, j = 0; i < input_length;) {

    uint32_t octet_a;
    uint32_t octet_b;
    uint32_t octet_c;
    uint32_t triple;
    octet_a = i < input_length ? (unsigned char)data[i++] : 0;
    octet_b = i < input_length ? (unsigned char)data[i++] : 0;
    octet_c = i < input_length ? (unsigned char)data[i++] : 0;

    triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

    encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
    encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
    encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
    encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
  }

  for (i = 0; i < mod_table[input_length % 3]; i++)
    encoded_data[output_length - 2 - i] = '=';

  encoded_data[output_length-1]=0;

  return encoded_data;
}


int mapcache_util_extract_int_list(mapcache_context *ctx, const char* cargs,
                                   const char *sdelim, int **numbers, int *numbers_count)
{
  char *last, *key, *endptr;
  char *args = apr_pstrdup(ctx->pool,cargs);
  int tmpcount=1;
  const char *delim = (sdelim)?sdelim:" ,\t\r\n";
  char sep;
  int i;
  *numbers_count = 0;
  i=strlen(delim);
  while(i--) {
    sep = delim[i];
    for(key=args; *key; key++) {
      if(*key == sep)
        tmpcount++;
    }
  }

  *numbers = (int*)apr_pcalloc(ctx->pool,tmpcount*sizeof(int));
  for (key = apr_strtok(args, delim, &last); key != NULL;
       key = apr_strtok(NULL, delim, &last)) {
    (*numbers)[(*numbers_count)++] = (int)strtol(key,&endptr,10);
    if(*endptr != 0)
      return MAPCACHE_FAILURE;
  }
  return MAPCACHE_SUCCESS;
}

int mapcache_util_extract_double_list(mapcache_context *ctx, const char* cargs,
                                      const char *sdelim, double **numbers, int *numbers_count)
{
  char *last, *key, *endptr;
  char *args = apr_pstrdup(ctx->pool,cargs);
  int tmpcount=1;
  const char *delim = (sdelim)?sdelim:" ,\t\r\n";
  char sep;
  int i;
  *numbers_count = 0;
  i=strlen(delim);
  while(i--) {
    sep = delim[i];
    for(key=args; *key; key++) {
      if(*key == sep)
        tmpcount++;
    }
  }
  *numbers = (double*)apr_pcalloc(ctx->pool,tmpcount*sizeof(double));
  for (key = apr_strtok(args, delim, &last); key != NULL;
       key = apr_strtok(NULL, delim, &last)) {
    (*numbers)[(*numbers_count)++] = strtod(key,&endptr);
    if(*endptr != 0)
      return MAPCACHE_FAILURE;
  }
  return MAPCACHE_SUCCESS;
}

char *mapcache_util_str_replace(apr_pool_t *pool, const char *string, const char *substr, const char *replacement )
{
  char *tok = NULL;
  char *newstr = NULL;

  tok = strstr( string, substr );
  if( tok == NULL ) return apr_pstrdup( pool, string );
  newstr = apr_pcalloc(pool, strlen( string ) - strlen( substr ) + strlen( replacement ) + 1 );
  memcpy( newstr, string, tok - string );
  memcpy( newstr + (tok - string), replacement, strlen( replacement ) );
  memcpy( newstr + (tok - string) + strlen( replacement ), tok + strlen( substr ), strlen( string ) - strlen( substr ) - ( tok - string ) );
  memset( newstr + strlen( string ) - strlen( substr ) + strlen( replacement ), 0, 1 );
  return newstr;
}

char* mapcache_util_str_sanitize(apr_pool_t *pool, const char *str, const char* from, char to)
{
  char *pstr = apr_pstrdup(pool,str);
  size_t pos = strcspn(pstr,from);
  if(pstr[pos]) {
    pstr = apr_pstrdup(pool,pstr);
    while(pstr[pos]) {
      ((char*)pstr)[pos]=to;
      pos += strcspn(&pstr[pos],from);
    }
  }
  return pstr;
}

#if APR_MAJOR_VERSION < 1 || (APR_MAJOR_VERSION < 2 && APR_MINOR_VERSION < 3)
APR_DECLARE(apr_table_t *) apr_table_clone(apr_pool_t *p, const apr_table_t *t)
{
  const apr_array_header_t *array = apr_table_elts(t);
  apr_table_entry_t *elts = (apr_table_entry_t *) array->elts;
  apr_table_t *new = apr_table_make(p, array->nelts);
  int i;

  for (i = 0; i < array->nelts; i++) {
    apr_table_add(new, elts[i].key, elts[i].val);
  }

  return new;
}

#endif

int _mapcache_context_get_error_default(mapcache_context *ctx)
{
  return ctx->_errcode;
}

char* _mapcache_context_get_error_msg_default(mapcache_context *ctx)
{
  return ctx->_errmsg;
}

void _mapcache_context_set_exception_default(mapcache_context *ctx, char *key, char *msg, ...)
{
  char *fullmsg;
  va_list args;
  if(!ctx->exceptions) {
    ctx->exceptions = apr_table_make(ctx->pool,1);
  }

  va_start(args,msg);
  fullmsg = apr_pvsprintf(ctx->pool,msg,args);
  va_end(args);
  apr_table_set(ctx->exceptions,key,fullmsg);
}

void _mapcache_context_set_error_default(mapcache_context *ctx, int code, char *msg, ...)
{
  char *new_msg;
  va_list args;
  va_start(args,msg);
  new_msg = apr_pvsprintf(ctx->pool,msg,args);
  va_end(args);

  if(ctx->_errmsg) {
    ctx->_errmsg = apr_pstrcat(ctx->pool, ctx->_errmsg, "\n", new_msg, NULL);
  } else {
    ctx->_errmsg = new_msg;
    ctx->_errcode = code;
  }
}

void _mapcache_context_clear_error_default(mapcache_context *ctx)
{
  ctx->_errcode = 0;
  ctx->_errmsg = NULL;
  if(ctx->exceptions) {
    apr_table_clear(ctx->exceptions);
  }
}


struct _error_log {
  int _errcode;
  char *_errmsg;
  apr_table_t *exceptions;
};

void _mapcache_context_pop_errors(mapcache_context *ctx, void **error)
{
  struct _error_log *e = (struct _error_log*)apr_pcalloc(ctx->pool, sizeof(struct _error_log));
  e->_errcode = ctx->_errcode;
  e->_errmsg = ctx->_errmsg;
  e->exceptions = ctx->exceptions;
  ctx->_errcode = 0;
  ctx->_errmsg = NULL;
  ctx->exceptions = NULL;
  *error = e;
}


void _mapcache_context_push_errors(mapcache_context *ctx, void *error)
{
  struct _error_log *e = (struct _error_log*)error;
  if(e->_errcode)
    ctx->_errcode = e->_errcode;
  if(e->_errmsg) {
    if(ctx->_errmsg) {
      ctx->_errmsg = apr_psprintf(ctx->pool,"%s\n%s",e->_errmsg,ctx->_errmsg);
    } else {
      ctx->_errmsg = e->_errmsg;
    }
  }
  if(e->exceptions) {
    if(ctx->exceptions) {
      apr_table_overlap(ctx->exceptions, e->exceptions, APR_OVERLAP_TABLES_SET);
    } else {
      ctx->exceptions = e->exceptions;
    }
  }
}


void mapcache_context_init(mapcache_context *ctx)
{
  ctx->_errcode = 0;
  ctx->_errmsg = NULL;
  ctx->get_error = _mapcache_context_get_error_default;
  ctx->get_error_message = _mapcache_context_get_error_msg_default;
  ctx->set_error = _mapcache_context_set_error_default;
  ctx->set_exception = _mapcache_context_set_exception_default;
  ctx->clear_errors = _mapcache_context_clear_error_default;
  ctx->pop_errors = _mapcache_context_pop_errors;
  ctx->push_errors = _mapcache_context_push_errors;
  ctx->headers_in = NULL;
}

void mapcache_context_copy(mapcache_context *src, mapcache_context *dst)
{
  dst->_contenttype = src->_contenttype;
  dst->_errcode = src->_errcode;
  dst->_errmsg = src->_errmsg;
  dst->clear_errors = src->clear_errors;
  dst->clone = src->clone;
  dst->config = src->config;
  dst->get_error = src->get_error;
  dst->get_error_message = src->get_error_message;
  dst->get_instance_id = src->get_instance_id;
  dst->log = src->log;
  dst->set_error = src->set_error;
  dst->pool = src->pool;
  dst->set_exception = src->set_exception;
  dst->service = src->service;
  dst->exceptions = src->exceptions;
  dst->threadlock = src->threadlock;
  dst->supports_redirects = src->supports_redirects;
  dst->pop_errors = src->pop_errors;
  dst->push_errors = src->push_errors;
  dst->connection_pool = src->connection_pool;
  dst->headers_in = src->headers_in;
}

char* mapcache_util_get_tile_dimkey(mapcache_context *ctx, mapcache_tile *tile, char* sanitized_chars, char *sanitize_to)
{
  char *key = apr_pstrdup(ctx->pool,"");
  if(tile->dimensions) {
    const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
    int i = elts->nelts;
    if(i>1) {
      while(i--) {
        apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
        if(i) {
          key = apr_pstrcat(ctx->pool,key,entry->val,(sanitized_chars?sanitize_to:"#"),NULL);
        } else {
          key = apr_pstrcat(ctx->pool,key,entry->val,NULL);
        }
      }
      return key;
    } else if(i) {
      apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,0,apr_table_entry_t));
      key = apr_pstrdup(ctx->pool,entry->val);
    }
    if(sanitized_chars)
      key = mapcache_util_str_sanitize(ctx->pool,key,sanitized_chars,*sanitize_to);
  }
  return key;
}

char* mapcache_util_get_tile_key(mapcache_context *ctx, mapcache_tile *tile, char *template,
                                 char* sanitized_chars, char *sanitize_to)
{
  char *path;
  if(template) {
    path = mapcache_util_str_replace(ctx->pool, template, "{x}",
                                     apr_psprintf(ctx->pool, "%d", tile->x));
    path = mapcache_util_str_replace(ctx->pool, path, "{y}",
                                     apr_psprintf(ctx->pool, "%d", tile->y));
    path = mapcache_util_str_replace(ctx->pool, path, "{z}",
                                     apr_psprintf(ctx->pool, "%d", tile->z));
    if(strstr(path,"{dim}")) {
      path = mapcache_util_str_replace(ctx->pool, path, "{dim}", mapcache_util_get_tile_dimkey(ctx,tile,sanitized_chars,sanitize_to));
    }
    if(strstr(path,"{tileset}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{tileset}", tile->tileset->name);
    if(strstr(path,"{grid}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{grid}", tile->grid_link->grid->name);
    if(strstr(path,"{ext}"))
      path = mapcache_util_str_replace(ctx->pool, path, "{ext}",
                                       tile->tileset->format ? tile->tileset->format->extension : "png");
  } else {
    char *separator = "/";
    /* we'll concatenate the entries ourself */
    path = apr_pstrcat(ctx->pool,
                       tile->tileset->name,separator,
                       tile->grid_link->grid->name,separator,
                       NULL);
    if(tile->dimensions) {
      path = apr_pstrcat(ctx->pool,path,
                         mapcache_util_get_tile_dimkey(ctx,tile,sanitized_chars,sanitize_to),
                         separator,NULL);
    }
    path = apr_pstrcat(ctx->pool,path,
                       apr_psprintf(ctx->pool, "%d", tile->z),separator,
                       apr_psprintf(ctx->pool, "%d", tile->y),separator,
                       apr_psprintf(ctx->pool, "%d", tile->x),separator,
                       tile->tileset->format?tile->tileset->format->extension:"png",
                       NULL);
  }
  return path;
}

void mapcache_make_parent_dirs(mapcache_context *ctx, char *filename) {
  char *hackptr1,*hackptr2=NULL;
  apr_status_t ret;
  char  errmsg[120];
  
  /* find the location of the last '/' in the string */
  hackptr1 = filename;
  while(*hackptr1) {
    if(*hackptr1 == '/')
      hackptr2 = hackptr1;
    hackptr1++;
  }
  
  if(hackptr2) {
    /* terminate string on last '/' */
    *hackptr2 = '\0';
  }

  ret = apr_dir_make_recursive(filename,APR_OS_DEFAULT,ctx->pool);
  
  if(hackptr2) {
    *hackptr2 = '/';
  }
  
  
  if(APR_SUCCESS != ret) {
    /*
     * apr_dir_make_recursive sometimes sends back this error, although it should not.
     * ignore this one
     */
    if(!APR_STATUS_IS_EEXIST(ret)) {
      ctx->set_error(ctx, 500, "failed to create directory %s: %s",filename, apr_strerror(ret,errmsg,120));
    }
  }
} 


#if defined(_WIN32) && !defined(__CYGWIN__)

int strncasecmp(const char *s1, const char *s2, int len)
{
  register const char *cp1, *cp2;
  int cmp = 0;

  cp1 = s1;
  cp2 = s2;

  if(len == 0)
    return(0);

  if (!*cp1)
    return -1;
  else if (!*cp2)
    return 1;

  while(*cp1 && *cp2 && len) {
    if((cmp = (toupper(*cp1) - toupper(*cp2))) != 0)
      return(cmp);
    cp1++;
    cp2++;
    len--;
  }

  if(len == 0) {
    return(0);
  }
  if(*cp1 || *cp2) {
    if (*cp1)
      return(1);
    else
      return (-1);
  }
  return(0);
}


#include <sys/timeb.h>
void mapcache_gettimeofday(struct mctimeval* tp, void* tzp)
{
  struct _timeb theTime;

  _ftime(&theTime);
  tp->tv_sec = theTime.time;
  tp->tv_usec = theTime.millitm * 1000;
}


#endif

/* vim: ts=2 sts=2 et sw=2
*/


