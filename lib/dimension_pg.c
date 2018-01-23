/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: PostgreSQL dimension support
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2018 Regents of the University of Minnesota.
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
#ifdef USE_POSTGRESQL
#include "libpq-fe.h"

typedef struct mapcache_dimension_postgresql mapcache_dimension_postgresql;

struct mapcache_dimension_postgresql {
  mapcache_dimension dimension;
  char *dbconnection;
  char *get_values_for_entry_query;
  char *get_all_values_query;
};

struct postgresql_dimension_conn {
  PGconn      *pgconn;     /* Connection to database */
  int statements[2]; /*prepared statement initialized?*/
};

void mapcache_postgresql_dimension_connection_constructor(mapcache_context *ctx, void **conn_, void *params)
{
  char *dbconn = (char*) params;
  struct postgresql_dimension_conn *conn = calloc(1, sizeof (struct postgresql_dimension_conn));
  *conn_ = conn;
  conn->pgconn = PQconnectdb(dbconn);
  /* Check to see that the backend connection was successfully made */
  if (PQstatus(conn->pgconn) != CONNECTION_OK) {
    ctx->set_error(ctx, 500, "failed to open postgresql connection: %s", PQerrorMessage(conn->pgconn));
    PQfinish(conn->pgconn);
    *conn_ = NULL;
    return;
  }
}

void mapcache_postgresql_dimension_connection_destructor(void *conn_)
{
  struct postgresql_dimension_conn *conn = (struct postgresql_dimension_conn*) conn_;
  PQfinish(conn->pgconn);
  free(conn);
}

static mapcache_pooled_connection* _postgresql_dimension_get_conn(mapcache_context *ctx, mapcache_tileset *tileset, mapcache_dimension_postgresql *dim) {
  mapcache_dimension *pdim = (mapcache_dimension*)dim;
  char *conn_key = apr_pstrcat(ctx->pool,"dim_",tileset?tileset->name:"","_",pdim->name,NULL);
  mapcache_pooled_connection *pc = mapcache_connection_pool_get_connection(ctx,conn_key,
        mapcache_postgresql_dimension_connection_constructor,
        mapcache_postgresql_dimension_connection_destructor, dim->dbconnection);
  return pc;
}

static void _postgresql_dimension_release_conn(mapcache_context *ctx, mapcache_pooled_connection *pc)
{
  if(GC_HAS_ERROR(ctx)) {
    mapcache_connection_pool_invalidate_connection(ctx,pc);
  } else {
    mapcache_connection_pool_release_connection(ctx,pc);
  }
}
#endif


mapcache_dimension* mapcache_dimension_postgresql_create(mapcache_context *ctx, apr_pool_t *pool)
{
#ifdef USE_POSTGRESQL
  mapcache_dimension_postgresql *dimension = apr_pcalloc(pool, sizeof(mapcache_dimension_postgresql));
  dimension->dimension.type = MAPCACHE_DIMENSION_POSTGRESQL;
  dimension->dimension.isTime = 0;
  dimension->dbconnection = NULL;
  dimension->dimension._get_entries_for_value = _mapcache_dimension_postgresql_get_entries_for_value;
  dimension->dimension._get_entries_for_time_range = _mapcache_dimension_postgresql_get_entries_for_time_range;
  dimension->dimension.configuration_parse_xml = _mapcache_dimension_postgresql_parse_xml;
  dimension->dimension.get_all_entries = _mapcache_dimension_postgresql_get_all_entries;
  dimension->dimension.get_all_ogc_formatted_entries = _mapcache_dimension_postgresql_get_all_entries;
  return (mapcache_dimension*)dimension;
#else
  ctx->set_error(ctx,400,"postgresql dimension support requires POSTGRESQL support to be built in");
  return NULL;
#endif
}