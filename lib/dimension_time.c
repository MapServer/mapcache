/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: time dimension support
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
#include <apr_time.h>
#include <apr_strings.h>
#include <time.h>

typedef enum {
  MAPCACHE_TINTERVAL_SECOND,
  MAPCACHE_TINTERVAL_MINUTE,
  MAPCACHE_TINTERVAL_HOUR,
  MAPCACHE_TINTERVAL_DAY,
  MAPCACHE_TINTERVAL_MONTH,
  MAPCACHE_TINTERVAL_YEAR
} mapcache_time_interval_t;

#ifndef HAVE_TIMEGM
time_t timegm(struct tm *tm)
{
  time_t t, tdiff;
  struct tm in, gtime, ltime;

  memcpy(&in, tm, sizeof(in));
  t = mktime(&in);

  memcpy(&gtime, gmtime(&t), sizeof(gtime));
  memcpy(&ltime, localtime(&t), sizeof(ltime));
  gtime.tm_isdst = ltime.tm_isdst;
  tdiff = t - mktime(&gtime);

  memcpy(&in, tm, sizeof(in));
  return mktime(&in) + tdiff;
}
#endif


char *mapcache_ogc_strptime(const char *value, struct tm *ts, mapcache_time_interval_t *ti) {
  char *valueptr;
  memset (ts, '\0', sizeof (*ts));
  ts->tm_mday = 1;
  valueptr = strptime(value,"%Y-%m-%dT%H:%M:%SZ",ts);
  *ti = MAPCACHE_TINTERVAL_SECOND;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%dT%H:%MZ",ts);
  *ti = MAPCACHE_TINTERVAL_MINUTE;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%dT%HZ",ts);
  *ti = MAPCACHE_TINTERVAL_HOUR;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m-%d",ts);
  *ti = MAPCACHE_TINTERVAL_DAY;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y-%m",ts);
  *ti = MAPCACHE_TINTERVAL_MONTH;
  if(valueptr) return valueptr;
  valueptr = strptime(value,"%Y",ts);
  *ti = MAPCACHE_TINTERVAL_YEAR;
  if(valueptr) return valueptr;
  return NULL;
}

apr_array_header_t* mapcache_dimension_time_get_entries(mapcache_context *ctx, mapcache_dimension *dim, const char *dim_value,
        mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid, time_t *intervals, int n_intervals) {
  int i;
  apr_array_header_t *time_ids = apr_array_make(ctx->pool,0,sizeof(char*));
  if(!dim->_get_entries_for_time_range) {
    ctx->set_error(ctx,500,"dimension does not support time queries");
    return NULL;
  }
  for(i=0;i<n_intervals;i++) {
      apr_array_header_t *interval_ids = dim->_get_entries_for_time_range(ctx, dim, dim_value,
            intervals[i*2], intervals[i*2+1],
            tileset, extent, grid);
      if(GC_HAS_ERROR(ctx)) {
        return NULL;
      }
      apr_array_cat(time_ids, interval_ids);
  }
  return time_ids;
}

apr_array_header_t* mapcache_dimension_time_get_entries_for_value(mapcache_context *ctx, mapcache_dimension *dimension, const char *value,
                                                                   mapcache_tileset *tileset, mapcache_extent *extent, mapcache_grid *grid) {
  
  /* look if supplied value is a predefined key */
  /* split multiple values, loop */

  /* extract start and end values */
  struct tm tm_start,tm_end;
  time_t *intervals;
  mapcache_time_interval_t tis,tie;
  char *valueptr = apr_pstrdup(ctx->pool,value);
  char *last,*key;
  int count=1;
  char * value_scan = (char *)value;
  
  /*count how many time entries were supplied*/
  for(; *value_scan; value_scan++) if(*value_scan == ',') count++;
  
  intervals = apr_pcalloc(ctx->pool,2*count*sizeof(time_t));
  count = 0;
  
  
  /* Split the input on ',' */
  for (key = apr_strtok(valueptr, ",", &last); key != NULL;
       key = apr_strtok(NULL, ",", &last)) {
    valueptr = mapcache_ogc_strptime(key,&tm_start,&tis);
    if(!valueptr) {
      ctx->set_error(ctx,400,"failed to parse time %s",value);
      return NULL;
    }

    if(*valueptr == '/' || (*valueptr == '-' && *(valueptr+1) == '-')) {
      /* we have a second (end) time */
      if (*valueptr == '/') {
        valueptr++;
      }
      else {
        valueptr += 2;
      }
      valueptr = mapcache_ogc_strptime(valueptr,&tm_end,&tie);
      if(!valueptr) {
        ctx->set_error(ctx,400,"failed to parse end time in %s",value);
        return NULL;
      }
    } else if(*valueptr == 0) {
      tie = tis;
      tm_end = tm_start;
    } else {
      ctx->set_error(ctx,400,"failed (2) to parse time %s",value);
      return NULL;
    }
    switch(tie) {
    case MAPCACHE_TINTERVAL_SECOND:
      tm_end.tm_sec += 1;
      break;
    case MAPCACHE_TINTERVAL_MINUTE:
      tm_end.tm_min += 1;
      break;
    case MAPCACHE_TINTERVAL_HOUR:
      tm_end.tm_hour += 1;
      break;
    case MAPCACHE_TINTERVAL_DAY:
      tm_end.tm_mday += 1;
      break;
    case MAPCACHE_TINTERVAL_MONTH:
      tm_end.tm_mon += 1;
      break;
    case MAPCACHE_TINTERVAL_YEAR:
      tm_end.tm_year += 1;
      break;
    }
    intervals[count*2] = timegm(&tm_start);
    intervals[count*2+1] = timegm(&tm_end);
    count++;
  }
  return mapcache_dimension_time_get_entries(ctx,dimension,value,tileset,extent,grid,intervals,count); 
  /* end loop */
}
