/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache utility program for preparing cache export
 * Author:   Jerome Boue and the MapServer team.
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

#include "mapcache_detail_config.h"
#if defined(USE_OGR) && defined(USE_GEOS)
#define USE_CLIPPERS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <float.h>

#ifndef _WIN32
#include <unistd.h>
#endif


#include <apr_general.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <sqlite3.h>
#include "mapcache.h"
#include "ezxml.h"
#include "cJSON.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <windows.h>
#else
#include <sys/ioctl.h>
#endif

#ifdef USE_CLIPPERS
#include <geos_c.h>
#include <ogr_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>
#endif // USE_CLIPPERS


///////////////////////////////////////////////////////////////////////////////
//
// Command Line Interface
//
///////////////////////////////////////////////////////////////////////////////

// List of command line options
const apr_getopt_option_t optlist[] = {
  { "help",           'h', FALSE, "Display this message and exit" },
  { "config",         'c', TRUE,  "Configuration file"
                                    " (/path/to/mapcache.xml)" },
  { "dimension",      'D', TRUE,  "Set the value of a dimension: format"
                                    " DIMENSIONNAME=VALUE. Can be used"
                                    " multiple times for multiple"
                                    " dimensions" },
  { "tileset",        't', TRUE,  "Tileset to analyze" },
  { "grid",           'g', TRUE,  "Grid to analyze" },
  { "extent",         'e', TRUE,  "Extent to analyze:"
                                    "format minx,miny,maxx,maxy. Cannot be"
                                    " used with --ogr-datasource." },
#ifdef USE_CLIPPERS
  { "ogr-datasource", 'd', TRUE,  "OGR data source to get features from."
                                    " Cannot be used with --extent." },
  { "ogr-layer",      'l', TRUE,  "OGR layer inside OGR data source. Cannot"
                                    " be used with --ogr-sql." },
  { "ogr-where",      'w', TRUE,  "Filter to apply on OGR layer features."
                                    " Cannot be used with --ogr-sql." },
  { "ogr-sql",        's', TRUE,  "SQL query to filter inside OGR data"
                                    " source. Cannot be used with"
                                    " --ogr-layer or --ogr-where." },
#endif // USE_CLIPPERS
  { "zoom",           'z', TRUE,  "Set min and max zoom levels to analyze,"
                                    " separated by a comma, eg: 12,15" },
  { "query",          'q', TRUE,  "Set query for counting tiles in a"
                                    " rectangle. Default value works with"
                                    " default schema of SQLite caches." },
  { "short-output",   'o', FALSE, "Only existing SQLite files are reported,"
                                    " missing SQLite files are still taken"
                                    " into account for level and global"
                                    " coverage." },
  { "endofopt",        0,  FALSE, "End of options" }
};

// Extract basename from a path
const char * base_name(const char * path)
{
  const char *subdir, *basename=path;
  for (subdir=path ; (subdir=strchr(subdir, '/')) ; basename=++subdir);
  return basename;
}

// Get terminal columns
int termcols()
{
#if defined(_WIN32) && !defined(__CYGWIN__)
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&csbi)) csbi.dwSize.X = 0;
  return csbi.dwSize.X;
#else
  struct winsize w;
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) < 0) w.ws_col = 0;
  return w.ws_col;
#endif
}

// Display help message
void usage(apr_pool_t * pool, const char * path, char * msg, ...)
{
  int i;
  const char * name = base_name(path);
  
  if (msg) {
    va_list args;
    va_start(args, msg);
    fprintf(stderr, "Error: %s: ", name);
    vfprintf(stderr, msg, args);
    fprintf(stderr, "\n\n");
    va_end(args);
  }

  fprintf(stderr, "\nUsage:      %s <options>\n\n", name);
  for (i=0 ; optlist[i].optch ; i++)
  {
    char ** words;
    int linewidth = 16;
    const int cols = termcols();

    fprintf(stderr, "    -%c | --%s%s\n                ",
        optlist[i].optch, optlist[i].name,
        optlist[i].has_arg ? " <value>" : "");
    apr_tokenize_to_argv(optlist[i].description, &words, pool);
    for (;*words;words++) {
      linewidth += strlen(*words)+1;
      if (cols > 0 && linewidth > cols) {
        fprintf(stderr, "\n                ");
        linewidth = 16 + strlen(*words)+1;
      }
      fprintf(stderr, "%s ", *words);
    }
    fprintf(stderr, "\n");
  }
}

///////////////////////////////////////////////////////////////////////////////
//
// Hooks for support libraries
//
///////////////////////////////////////////////////////////////////////////////

#ifdef USE_CLIPPERS
// GEOS notice function
void notice(const char *fmt,...)
{
  va_list ap;
  fprintf(stdout,"{ notice: ");
  va_start(ap,fmt);
  vfprintf(stdout,fmt,ap);
  va_end(ap);
  fprintf(stdout," }\n");
}

// GEOS log_and_exit function
void log_and_exit(const char *fmt,...)
{
  va_list ap;
  fprintf(stderr,"{ error: ");
  va_start(ap,fmt);
  vfprintf(stderr,fmt,ap);
  va_end(ap);
  fprintf(stderr," }\n");
  exit(1);
}
#endif // USE_CLIPPERS

// Mapcache log function
void mapcache_log(mapcache_context *ctx, mapcache_log_level lvl, char *msg, ...)
{
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  fprintf(stderr, "\n");
}

// cJSON memory allocation Hook on APR pool mechanism
static apr_pool_t * _pool_for_cJSON_malloc_hook;
static void * _malloc_for_cJSON(size_t size) {
  return apr_palloc(_pool_for_cJSON_malloc_hook,size);
}
static void _free_for_cJSON(void *ptr) { }
static void _create_json_pool(apr_pool_t * parent_pool) {
  cJSON_Hooks hooks = { _malloc_for_cJSON, _free_for_cJSON };
  apr_pool_create(&_pool_for_cJSON_malloc_hook,parent_pool);
  cJSON_InitHooks(&hooks);
}
static void _destroy_json_pool() {
  apr_pool_destroy(_pool_for_cJSON_malloc_hook);
}


///////////////////////////////////////////////////////////////////////////////
//
// Conversions between various geometry formats
//
///////////////////////////////////////////////////////////////////////////////

#ifdef USE_CLIPPERS
// Convert `mapcache_extent` to `GEOSGeometry` polygon
GEOSGeometry * mapcache_extent_to_GEOSGeometry(const mapcache_extent *extent)
{
  GEOSCoordSequence *cs = GEOSCoordSeq_create(5,2);
  GEOSGeometry *lr = GEOSGeom_createLinearRing(cs);
  GEOSGeometry *bb = GEOSGeom_createPolygon(lr,NULL,0);
  GEOSCoordSeq_setX(cs,0,extent->minx);
  GEOSCoordSeq_setY(cs,0,extent->miny);
  GEOSCoordSeq_setX(cs,1,extent->maxx);
  GEOSCoordSeq_setY(cs,1,extent->miny);
  GEOSCoordSeq_setX(cs,2,extent->maxx);
  GEOSCoordSeq_setY(cs,2,extent->maxy);
  GEOSCoordSeq_setX(cs,3,extent->minx);
  GEOSCoordSeq_setY(cs,3,extent->maxy);
  GEOSCoordSeq_setX(cs,4,extent->minx);
  GEOSCoordSeq_setY(cs,4,extent->miny);
  return bb;
}

// Update `GEOSGeometry` polygon created with
// `mapcache_extent_to_GEOSGeometry()` with values from another
// `mapcache_extent`
void update_GEOSGeometry_with_mapcache_extent(GEOSGeometry *bb,
                                              const mapcache_extent *extent)
{
  GEOSGeometry *lr = (GEOSGeometry *)GEOSGetExteriorRing(bb);
  GEOSCoordSequence *cs = (GEOSCoordSequence *)GEOSGeom_getCoordSeq(lr);
  GEOSCoordSeq_setX(cs,0,extent->minx);
  GEOSCoordSeq_setY(cs,0,extent->miny);
  GEOSCoordSeq_setX(cs,1,extent->maxx);
  GEOSCoordSeq_setY(cs,1,extent->miny);
  GEOSCoordSeq_setX(cs,2,extent->maxx);
  GEOSCoordSeq_setY(cs,2,extent->maxy);
  GEOSCoordSeq_setX(cs,3,extent->minx);
  GEOSCoordSeq_setY(cs,3,extent->maxy);
  GEOSCoordSeq_setX(cs,4,extent->minx);
  GEOSCoordSeq_setY(cs,4,extent->miny);
}

// Convert `GEOSGeometry` to GeoJSON string
char * GEOSGeometry_to_GeoJSON(const GEOSGeometry *g)
{
  OGRGeometryH geom;
  GEOSWKTWriter * wr = GEOSWKTWriter_create();
  char * wkt = GEOSWKTWriter_write(wr,g);
  char * geojson = NULL;
  OGR_G_CreateFromWkt(&wkt,NULL,&geom);
  geojson = OGR_G_ExportToJson(geom);
  OGR_G_DestroyGeometry(geom);
  GEOSWKTWriter_destroy(wr);
  return geojson;
}

// Convert `GEOSGeometry` to `cJSON` structure embedding GeoJSON data
cJSON * GEOSGeometry_to_cJSON(const GEOSGeometry *g)
{
  char * geojson = GEOSGeometry_to_GeoJSON(g);
  cJSON * jgeom = cJSON_Parse(geojson);
  CPLFree(geojson);
  return jgeom;
}

// Convert `mapcache_extent` to GeoJSON string
char * mapcache_extent_to_GeoJSON(const mapcache_extent *extent)
{
  return GEOSGeometry_to_GeoJSON(mapcache_extent_to_GEOSGeometry(extent));
}

// Convert `mapcache_extent` to `cJSON` structure embedding GeoJSON data
cJSON * mapcache_extent_to_cJSON(const mapcache_extent *extent)
{
  return GEOSGeometry_to_cJSON(mapcache_extent_to_GEOSGeometry(extent));
}

// Convert `OGRLayerH` to `cJSON` structure embedding GeoJSON data
cJSON * OGRLayerH_to_cJSON(const OGRLayerH layer)
{
  cJSON *jlayer, *jfeatures, *jfeature, *jgeom;
  OGRFeatureH feature;
  int nfeat = 0;

  jlayer = cJSON_CreateObject();
  cJSON_AddStringToObject(jlayer, "type", "FeatureCollection");
  jfeatures = cJSON_AddArrayToObject(jlayer, "features");

  OGR_L_ResetReading(layer);
  while ((feature = OGR_L_GetNextFeature(layer))) {
    OGRGeometryH ogr_geom = OGR_F_GetGeometryRef(feature);
    char * geojson = OGR_G_ExportToJson(ogr_geom);
    jgeom = cJSON_Parse(geojson);
    CPLFree(geojson);
    jfeature = cJSON_CreateObject();
    cJSON_AddItemToObject(jfeatures, "", jfeature);
    cJSON_AddStringToObject(jfeature, "type", "Feature");
    cJSON_AddItemToObject(jfeature, "geometry", jgeom);
    nfeat++;
  }

  if (nfeat == 1) {
    return jgeom;
  } else {
    return jlayer;
  }
}
#else
// Convert `mapcache_extent` to `cJSON` structure embedding GeoJSON data
cJSON * mapcache_extent_to_cJSON(const mapcache_extent *extent)
{
  cJSON * geojson = cJSON_CreateObject();
  cJSON * jpolygon;
  cJSON * jpoint;

  cJSON_AddStringToObject(geojson, "type", "Polygon");
  jpolygon = cJSON_AddArrayToObject(geojson, "coordinates");

  jpoint = cJSON_AddArrayToObject(jpolygon, "");
  cJSON_AddNumberToObject(jpoint, "", extent->minx);
  cJSON_AddNumberToObject(jpoint, "", extent->miny);

  jpoint = cJSON_CreateObject();
  jpoint = cJSON_AddArrayToObject(jpolygon, "");
  cJSON_AddNumberToObject(jpoint, "", extent->maxx);
  cJSON_AddNumberToObject(jpoint, "", extent->miny);

  jpoint = cJSON_CreateObject();
  jpoint = cJSON_AddArrayToObject(jpolygon, "");
  cJSON_AddNumberToObject(jpoint, "", extent->maxx);
  cJSON_AddNumberToObject(jpoint, "", extent->maxy);

  jpoint = cJSON_CreateObject();
  jpoint = cJSON_AddArrayToObject(jpolygon, "");
  cJSON_AddNumberToObject(jpoint, "", extent->minx);
  cJSON_AddNumberToObject(jpoint, "", extent->maxy);

  jpoint = cJSON_CreateObject();
  jpoint = cJSON_AddArrayToObject(jpolygon, "");
  cJSON_AddNumberToObject(jpoint, "", extent->minx);
  cJSON_AddNumberToObject(jpoint, "", extent->miny);

  return geojson;
}
#endif // USE_CLIPPERS


///////////////////////////////////////////////////////////////////////////////
//
// Operations on Mapcache SQLite files
//
///////////////////////////////////////////////////////////////////////////////

// Replace all occurrences of substr in string
char * str_replace_all(apr_pool_t *pool, const char *string,
                       const char *substr, const char *replacement)
{
  char * replaced = apr_pstrdup(pool, string);
  while (strstr(replaced, substr)) {
    replaced = mapcache_util_str_replace(pool, string, substr, replacement);
  }
  return replaced;
}

// Build up actual SQLite filename from dbfile template
char * dbfilename(apr_pool_t * pool, char * template,
                  mapcache_tileset * tileset, mapcache_grid * grid,
                  apr_array_header_t * dimensions, apr_hash_t * fmt, int z,
                  int dbx, int dby, int xcount, int ycount)
{
  int tilx = dbx * xcount;
  int tily = dby * ycount;
  char * path = apr_pstrdup(pool, template);
  if (!strstr(path, "{")) {
    return path;
  }

  // Tileset and grid
  path = str_replace_all(pool, path, "{tileset}", tileset->name);
  path = str_replace_all(pool, path, "{grid}", grid->name);

  // Dimensions: both {dim} and {dim:foo}
  if (strstr(path, "{dim") && dimensions) {
    char * dimstr = "";
    int i = dimensions->nelts;
    while(i--) {
      mapcache_requested_dimension *entry;
      const char * val;
      char *solodim;
      entry = APR_ARRAY_IDX(dimensions, i, mapcache_requested_dimension*);
      val = mapcache_util_str_sanitize(pool, entry->cached_value, "/.", '#');
      solodim = apr_pstrcat(pool, "{dim:", entry->dimension->name, "}", NULL);
      dimstr = apr_pstrcat(pool, dimstr, "#", val, NULL);
      if(strstr(path, solodim)) {
        path = str_replace_all(pool, path, solodim, val);
      }
    }
    path = str_replace_all(pool, path, "{dim}", dimstr);
  }


  // Zoom level
  path = str_replace_all(pool, path, "{z}",
      apr_psprintf(pool, apr_hash_get(fmt, "z", APR_HASH_KEY_STRING), z));

  // X coordinate
  if (xcount > 0) {
    char * curfmt;
    curfmt = apr_hash_get(fmt, "x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{x}",
        apr_psprintf(pool, curfmt, tilx));
    curfmt = apr_hash_get(fmt, "div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_x}",
        apr_psprintf(pool, curfmt, dbx));
    curfmt = apr_hash_get(fmt, "inv_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_x}",
        apr_psprintf(pool, curfmt, tilx));
    curfmt = apr_hash_get(fmt, "inv_div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_x}",
        apr_psprintf(pool, curfmt, dbx));
  }

  // Y coordinate
  if (ycount > 0) {
    char * curfmt;
    curfmt = apr_hash_get(fmt, "y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{y}",
        apr_psprintf(pool, curfmt, tily));
    curfmt = apr_hash_get(fmt, "div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_y}",
        apr_psprintf(pool, curfmt, dby));
    curfmt = apr_hash_get(fmt, "inv_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_y}",
        apr_psprintf(pool, curfmt, tily));
    curfmt = apr_hash_get(fmt, "inv_div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_y}",
        apr_psprintf(pool, curfmt, dby));
  }

  return path;
}


// Query SQLite for getting tile count in specified area
void count_tiles_in_rectangle(
    mapcache_context * ctx,
    int z,
    mapcache_extent_i til,
    mapcache_tileset * tileset,
    mapcache_grid_link * grid_link,
    apr_array_header_t * dimensions,
    const char * dbfile,
    const char * count_query,
    int *tmax,
    int *tcached
    )
{
  mapcache_grid *grid = grid_link->grid;
  sqlite3 * db;
  sqlite3_stmt * res;
  int rc, idx;
  int count;

  *tcached = 0;
  *tmax = (til.maxx-til.minx+1) * (til.maxy-til.miny+1);

  rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return;
  }
  sqlite3_busy_timeout(db, 5000);

  rc = sqlite3_prepare_v2(db, count_query, -1, &res, 0);
  if (rc != SQLITE_OK) {
    ctx->set_error(ctx, 500, "SQLite failed on %s: '%s'",
        dbfile, sqlite3_errmsg(db));
    sqlite3_close(db);
    return;
  }

  idx = sqlite3_bind_parameter_index(res, ":minx");
  if (idx) sqlite3_bind_int(res, idx, til.minx);
  idx = sqlite3_bind_parameter_index(res, ":miny");
  if (idx) sqlite3_bind_int(res, idx, til.miny);
  idx = sqlite3_bind_parameter_index(res, ":maxx");
  if (idx) sqlite3_bind_int(res, idx, til.maxx);
  idx = sqlite3_bind_parameter_index(res, ":maxy");
  if (idx) sqlite3_bind_int(res, idx, til.maxy);
  idx = sqlite3_bind_parameter_index(res, ":z");
  if (idx) sqlite3_bind_int(res, idx, z);
  idx = sqlite3_bind_parameter_index(res, ":grid");
  if (idx) sqlite3_bind_text(res, idx, grid->name, -1, SQLITE_STATIC);
  idx = sqlite3_bind_parameter_index(res, ":tileset");
  if (idx) sqlite3_bind_text(res, idx, tileset->name, -1, SQLITE_STATIC);
  idx = sqlite3_bind_parameter_index(res, ":dim");
  if (idx) {
    char * dim = "";
    if (dimensions) {
      mapcache_tile tile;
      tile.dimensions = dimensions;
      dim = mapcache_util_get_tile_dimkey(ctx, &tile, NULL, NULL);
    }
    sqlite3_bind_text(res, idx, dim, -1, SQLITE_STATIC);
  }

  rc = sqlite3_step(res);
  switch (rc) {
    case SQLITE_ROW:
      count = strtol((const char*)sqlite3_column_text(res, 0), NULL, 10);
      break;
    case SQLITE_DONE:
      ctx->set_error(ctx, 500, "SQLite returned no tile count (%s)", dbfile);
      count = 0;
      break;
    default:
      ctx->set_error(ctx, 500, "SQLite failed on %s: '%s'",
          dbfile, sqlite3_errmsg(db));
      count = 0;
  }

  sqlite3_finalize(res);
  sqlite3_close(db);

  *tcached = count;
}


#ifdef USE_CLIPPERS
// Count tiles in arbitrary region geometry for given zoom level, iterating
// over supplied bounding box expressed in tiles
void count_tiles_in_region(
    mapcache_context *c,
    int zl,
    mapcache_extent_i til_bbox,
    mapcache_tileset *t,
    mapcache_grid_link *gl,
    apr_array_header_t *d,
    const GEOSPreparedGeometry *pg,
    int file_exists,
    int *tmax,
    int *tcached
    )
{
  int tx, ty;
  GEOSGeometry *tile_geom = NULL;

  // Iterate over tiles within supplied bbox
  *tmax = *tcached = 0;
  for (tx = til_bbox.minx ; tx <= til_bbox.maxx ; tx++) {
    for (ty = til_bbox.miny ; ty <= til_bbox.maxy ; ty++) {
      mapcache_extent tile_bbox;

      // Compute tile bounding box expressed in grid units
      mapcache_grid_get_tile_extent(c, gl->grid, tx, ty, zl, &tile_bbox);
      if (!tile_geom) {
        tile_geom = mapcache_extent_to_GEOSGeometry(&tile_bbox);
      } else {
        update_GEOSGeometry_with_mapcache_extent(tile_geom, &tile_bbox);
      }

      // Count tile if it is within region of interest
      if (GEOSPreparedIntersects(pg, tile_geom) == 1) {
        int tile_exists = FALSE;
        mapcache_tile *tile;

        (*tmax)++;

        // Check whether tile is present in cache
        if (file_exists) {
          tile = mapcache_tileset_tile_create(c->pool, t, gl);
          tile->x = tx;
          tile->y = ty;
          tile->z = zl;
          tile->dimensions = d;
          tile_exists = mapcache_cache_tile_exists(c, t->_cache, tile);
          if (tile_exists) (*tcached)++;
        }
      }
    }
  }
  GEOSGeom_destroy(tile_geom);
}
#endif // USE_CLIPPERS


///////////////////////////////////////////////////////////////////////////////
//
// Program entry point
//
///////////////////////////////////////////////////////////////////////////////

int main(int argc, char * argv[])
{
  mapcache_context ctx;
  apr_getopt_t * opt;
  int status;
  int optk;
  const char * optv;
  int json_output = TRUE;
  int show_progress = TRUE;
  cJSON *jreport, *jitem, *jregion, *jzooms, *jzoom, *jfiles, *jfile;
  const char * config_file = NULL;
  const char * tileset_name = NULL;
  const char * grid_name = NULL;
  const char * dim_spec = NULL;
  const char * count_query = NULL;
  const char * extent = NULL;
  const char * ogr_file = NULL;
  const char * ogr_layer = NULL;
  const char * ogr_where = NULL;
  const char * ogr_sql = NULL;
  const char * zoom = NULL;
#ifdef USE_CLIPPERS
  GEOSGeometry *region_geom;
  GEOSGeometry *grid_geom;
  const GEOSPreparedGeometry *region_prepgeom = NULL;
#else // Use region_bbox instead of region_geom
#endif // USE_CLIPPERS
  mapcache_tileset * tileset = NULL;
  mapcache_grid * grid = NULL;
  mapcache_grid_link * grid_link;
  apr_array_header_t * dimensions = NULL;
  struct cache_info {
    ezxml_t node;
    mapcache_cache * cache;
    int minzoom, maxzoom;
    char * dbfile;
    apr_hash_t * formats;
    int xcount, ycount;
  } *cache;
  apr_array_header_t * caches = NULL;
  int i, ix, iy, iz;
  ezxml_t doc, node;
  ezxml_t dbfile_node = NULL;
  char * text;
  apr_hash_index_t * hi;
  mapcache_extent region_bbox = { 0, 0, 0, 0 };
  double * list = NULL;
  int nelts;
  int minzoom = 0, maxzoom = 0;
  int64_t tiles_max_in_cache = 0;
  int64_t tiles_cached_in_cache = 0;
  apr_off_t size_of_cache = 0;
  int64_t tiles_in_cache = 0;
  apr_hash_t * db_files = NULL;
  int cid;
  int nb_missing_files = 0;
  int report_missing_files = TRUE;


  /////////////////////////////////////////////////////////////////////////////
  // Initialize Apache, GEOS, OGR, cJSON and Mapcache
  //
  apr_initialize();
#ifdef USE_CLIPPERS
  initGEOS(notice, log_and_exit);
  OGRRegisterAll();
#endif // USE_CLIPPERS
  apr_pool_create(&ctx.pool, NULL);
  _create_json_pool(ctx.pool);
  mapcache_context_init(&ctx);
  ctx.config = mapcache_configuration_create(ctx.pool);
  ctx.log = mapcache_log;
  mapcache_connection_pool_create(&ctx.connection_pool, ctx.pool);


  /////////////////////////////////////////////////////////////////////////////
  // Parse command-line options
  //
  apr_getopt_init(&opt, ctx.pool, argc, (const char*const*)argv);
  while ((status = apr_getopt_long(opt, optlist, &optk, &optv)) == APR_SUCCESS)
  {
    switch (optk) {
      case 'h': // --help
        usage(ctx.pool, argv[0], NULL);
        goto success;
        break;
      case 'c': // --config <config_file>
        config_file = optv;
        break;
      case 't': // --tileset <tileset_name>
        tileset_name = optv;
        break;
      case 'g': // --grid <grid_name>
        grid_name = optv;
        break;
      case 'D': // --dimension <dim_spec>
        if (!dim_spec) {
          dim_spec = optv;
        } else {
          dim_spec = apr_pstrcat(ctx.pool, dim_spec, ":", optv, NULL);
        }
        break;
      case 'q': // --query <count_query>
        count_query = optv;
        break;
      case 'e': // --extent <minx>,<miny>,<maxx>,<maxy>
        extent = optv;
        break;
      case 'd': // --ogr-datasource <ogr_file>
        ogr_file = optv;
        break;
      case 'l': // --ogr-layer <ogr_layer>
        ogr_layer = optv;
        break;
      case 'w': // --ogr-where <ogr_where>
        ogr_where = optv;
        break;
      case 's': // --ogr-sql <ogr_sql>
        ogr_sql = optv;
        break;
      case 'z': // --zoom <minz>[,<maxz>]
        zoom = optv;
        break;
      case 'o': // --short-output
        report_missing_files = FALSE;
        break;
    }
  }
  if (status != APR_EOF) {
    usage(ctx.pool, argv[0], "Bad options");
    goto failure;
  }


  /////////////////////////////////////////////////////////////////////////////
  // Load Mapcache configuration file in Mapcache internal data structure
  //
  if (!config_file) {
    usage(ctx.pool, argv[0], "Configuration file has not been specified");
    goto failure;
  }
  mapcache_configuration_parse(&ctx, config_file, ctx.config, 0);
  if (GC_HAS_ERROR(&ctx)) goto failure;


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve tileset information
  //
  if (!tileset_name) {
    usage(ctx.pool, argv[0], "Tileset has not been specified");
    goto failure;
  }
  tileset = mapcache_configuration_get_tileset(ctx.config, tileset_name);
  if (!tileset) {
    ctx.set_error(&ctx, 500,
        "Tileset \"%s\" has not been found in configuration \"%s\"",
        tileset_name, config_file);
    goto failure;
  }


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve grid information
  //
  if (!grid_name) {
    usage(ctx.pool, argv[0], "Grid has not been specified");
    goto failure;
  }
  for (i=0 ; i<tileset->grid_links->nelts ; i++) {
    grid_link = APR_ARRAY_IDX(tileset->grid_links, i, mapcache_grid_link*);
    if (strcmp(grid_link->grid->name, grid_name)==0) {
      grid = grid_link->grid;
      break;
    }
  }
  if (!grid) {
    ctx.set_error(&ctx, 500,
        "Grid \"%s\" has not been found in tileset \"%s\"",
        grid_name, tileset->name);
    goto failure;
  }


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve region of interest geometry, either from --extent or from
  // --ogr-... option group
  //
  if (extent && ogr_file) {
    ctx.set_error(&ctx, 500,
        "Extent and OGR Data Source are mutually exclusive");
    goto failure;
  }
  if (!ogr_file && (ogr_sql || ogr_layer || ogr_where)) {
    ctx.set_error(&ctx, 500,
        "OGR Data Source is required with other OGR related options");
    goto failure;
  }
  if (ogr_sql && (ogr_layer || ogr_where)) {
    ctx.set_error(&ctx, 500,
        "--ogr-sql cannot be used with --ogr-layer or --ogr-where");
    goto failure;
  }

  // Region of interest is specified with --extent
  if (extent) {
#ifdef USE_CLIPPERS
    GEOSGeometry *temp;
#endif
    if (mapcache_util_extract_double_list(&ctx, extent, ",", &list, &nelts)
        != MAPCACHE_SUCCESS || nelts != 4)
    {
      usage(ctx.pool, argv[0], "Failed to parse extent: \"%s\"", extent);
      goto failure;
    }
    region_bbox.minx = list[0];
    region_bbox.miny = list[1];
    region_bbox.maxx = list[2];
    region_bbox.maxy = list[3];
    // Swap bounds if inverted
    if (region_bbox.minx > region_bbox.maxx) {
      double swap = region_bbox.minx;
      region_bbox.minx = region_bbox.maxx;
      region_bbox.maxx = swap;
    }
    if (region_bbox.miny > region_bbox.maxy) {
      double swap = region_bbox.miny;
      region_bbox.miny = region_bbox.maxy;
      region_bbox.maxy = swap;
    }
#ifdef USE_CLIPPERS
    temp = mapcache_extent_to_GEOSGeometry(&region_bbox);
    region_geom = temp;
#else // Use region_bbox instead of region_geom
#endif
  }

#ifdef USE_CLIPPERS
  // Region of interest is specified with OGR
  if (ogr_file) {
    OGRDataSourceH ogr = NULL;
    OGRLayerH layer = NULL;
    OGRFeatureH feature;
    GEOSWKTReader *geoswktreader;
    int nFeatures;

    ogr = OGROpen(ogr_file, FALSE, NULL);
    if (!ogr) {
      ctx.set_error(&ctx, 500, "Failed to open OGR data source: %s", ogr_file);
      goto failure;
    }

    // Get layer from OGR data source
    if (ogr_sql) {
      // Get layer from SQL
      layer = OGR_DS_ExecuteSQL(ogr,ogr_sql, NULL, NULL);
      if (!layer) {
        ctx.set_error(&ctx, 500, "Failed to get OGR layer from OGR SQL query");
      }
    } else {
      // Get layer from OGR layer / OGR where
      int nlayers = OGR_DS_GetLayerCount(ogr);
      if (nlayers > 1 && !ogr_layer) {
        ctx.set_error(&ctx, 500, "OGR data source has more than one layer"
                                 " but OGR layer has not been specified");
        goto failure;
      } else if (ogr_layer) {
        layer = OGR_DS_GetLayerByName(ogr, ogr_layer);
      } else {
        layer = OGR_DS_GetLayer(ogr, 0);
      }
      if (!layer) {
        ctx.set_error(&ctx, 500, "Failed to find OGR layer");
        goto failure;
      }
      if (ogr_where) {
        if (OGR_L_SetAttributeFilter(layer, ogr_where) != OGRERR_NONE) {
          ctx.set_error(&ctx, 500, "Failed to filter with --ogr-where %s",
                        ogr_where);
          goto failure;
        }
      }
    }
    OGR_L_ResetReading(layer);

    // Get geometry from layer
    nFeatures = OGR_L_GetFeatureCount(layer, TRUE);
    if (nFeatures == 0) {
      ctx.set_error(&ctx, 500, "Failed to find features in OGR layer");
      goto failure;
    }
    region_geom = GEOSGeom_createEmptyPolygon();
    geoswktreader = GEOSWKTReader_create();
    nFeatures = 0;
    while ((feature = OGR_L_GetNextFeature(layer))) {
      char *wkt;
      GEOSGeometry *geos_geom, *temp;
      OGREnvelope ogr_extent;
      OGRGeometryH ogr_geom = OGR_F_GetGeometryRef(feature);
      if (!ogr_geom || !OGR_G_IsValid(ogr_geom)) continue;
      OGR_G_ExportToWkt(ogr_geom,&wkt);
      geos_geom = GEOSWKTReader_read(geoswktreader,wkt);
      CPLFree(wkt);
      OGR_G_GetEnvelope(ogr_geom, &ogr_extent);
      if (nFeatures == 0) {
        region_bbox.minx = ogr_extent.MinX;
        region_bbox.miny = ogr_extent.MinY;
        region_bbox.maxx = ogr_extent.MaxX;
        region_bbox.maxy = ogr_extent.MaxY;
      } else {
        region_bbox.minx = fminl(region_bbox.minx, ogr_extent.MinX);
        region_bbox.miny = fminl(region_bbox.miny, ogr_extent.MinY);
        region_bbox.maxx = fmaxl(region_bbox.maxx, ogr_extent.MaxX);
        region_bbox.maxy = fmaxl(region_bbox.maxy, ogr_extent.MaxY);
      }
      nFeatures++;
      temp = GEOSUnion(region_geom, geos_geom);
      GEOSGeom_destroy(region_geom);
      region_geom = temp;
    }
    GEOSWKTReader_destroy(geoswktreader);
  }
#endif // USE_CLIPPERS

  // Set region to grid extent when no region has been provided
  if (!extent && !ogr_file) {
#ifdef USE_CLIPPERS
    GEOSGeometry *temp;
#endif
    region_bbox.minx = grid->extent.minx;
    region_bbox.miny = grid->extent.miny;
    region_bbox.maxx = grid->extent.maxx;
    region_bbox.maxy = grid->extent.maxy;
#ifdef USE_CLIPPERS
    temp = mapcache_extent_to_GEOSGeometry(&region_bbox);
    region_geom = temp;
#else // Use region_bbox instead of region_geom
#endif
  }

  // Region of interest must be within grid extent
#ifdef USE_CLIPPERS
  region_prepgeom = GEOSPrepare(region_geom);
  grid_geom = mapcache_extent_to_GEOSGeometry(&(grid->extent));
  if (GEOSPreparedWithin(region_prepgeom, grid_geom) != 1) {
#else // Use region_bbox instead of region_geom
  if (   (region_bbox.minx < grid->extent.minx)
      || (region_bbox.miny < grid->extent.miny)
      || (region_bbox.maxx > grid->extent.maxx)
      || (region_bbox.maxy > grid->extent.maxy))
  {
#endif // USE_CLIPPERS
    ctx.set_error(&ctx, 500,
        "Requested geometry is not contained within Grid extent: "
        "[ %.18g, %.18g, %.18g, %.18g ]\n", grid->extent.minx,
        grid->extent.miny, grid->extent.maxx, grid->extent.maxy);
    goto failure;
  }


  /////////////////////////////////////////////////////////////////////////////
  // Load MapCache configuration again, this time as an XML document, in order
  // to gain access to settings that are unreacheable from Mapcache API
  //
  doc = ezxml_parse_file(config_file);


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve cache information from XML document
  //

  // Create table of caches
  if (!caches) caches = apr_array_make(ctx.pool, 1, sizeof(void*));

  // Retrieve cache element in XML configuration
  cache = apr_palloc(ctx.pool, sizeof(struct cache_info));
  cache->cache = tileset->_cache;
  cache->node = NULL;
  for (node = ezxml_child(doc, "cache") ; node ; node = node->next) {
    if (strcmp(ezxml_attr(node, "name"), cache->cache->name) == 0) {
      cache->node = node;
      break;
    }
  }

  // Retrieve individuals caches in the composite
  if (cache->cache->type == MAPCACHE_CACHE_COMPOSITE) {
    for (node = ezxml_child(cache->node, "cache") ; node ; node = node->next) {
      struct cache_info * c = apr_palloc(ctx.pool, sizeof(struct cache_info));
      const char * val;
      ezxml_t n;
      // Read min-zoom and max-zoom attributes of <cache> reference
      c->minzoom = 0;
      c->maxzoom = INT_MAX;
      val = ezxml_attr(node, "min-zoom");
      if (val) c->minzoom = (int)strtol(val, NULL, 10);
      val = ezxml_attr(node, "max-zoom");
      if (val) c->maxzoom = (int)strtol(val, NULL, 10);
      // Retrieve individual cache element in XML configuration
      c->cache = mapcache_configuration_get_cache(ctx.config, node->txt);
      c->node = NULL;
      for (n = ezxml_child(doc, "cache") ; n ; n = n->next) {
        if (strcmp(ezxml_attr(n, "name"), c->cache->name) == 0) {
          c->node = n;
          break;
        }
      }
      APR_ARRAY_PUSH(caches, struct cache_info*) = c;
    }
  }
  else {
    APR_ARRAY_PUSH(caches, struct cache_info*) = cache;
  }

  // Retrieve information on all stored caches
  for ( cid=0 ; cid < caches->nelts ; cid++ ) {
    struct cache_info * c = APR_ARRAY_IDX(caches, cid, struct cache_info*);

    // Only SQLite caches are handled
    if (c->cache->type != MAPCACHE_CACHE_SQLITE) {
      ctx.set_error(&ctx, 500,
          "cache \"%s\" of tileset \"%s\" is not of type SQLite",
          c->cache->name, tileset->name);
      goto failure;
    }

    // Read <dbfile> element from <cache>
    dbfile_node = ezxml_child(c->node, "dbfile");
    c->dbfile = dbfile_node->txt;
    if (!c->dbfile) {
      ctx.set_error(&ctx, 500,
          "Failed to parse <dbfile> tag of cache \"%s\"", c->cache->name);
      goto failure;
    }

    // Read formats of x,y,z placeholders in dbfile template
    c->formats = apr_hash_make(ctx.pool);
    apr_hash_set(c->formats, "x",         APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "y",         APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "z",         APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "inv_x",     APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "inv_y",     APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "div_x",     APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "div_y",     APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "inv_div_x", APR_HASH_KEY_STRING, "(not set)");
    apr_hash_set(c->formats, "inv_div_y", APR_HASH_KEY_STRING, "(not set)");
    for (hi = apr_hash_first(ctx.pool, c->formats)
        ; hi
        ; hi = apr_hash_next(hi))
    {
      const char *key, *val, *attr;
      apr_hash_this(hi, (const void**)&key, NULL, (void**)&val);
      attr = apr_pstrcat(ctx.pool, key, "_fmt", NULL);
      val = ezxml_attr(dbfile_node, attr);
      if (!val) val = "%d";
      apr_hash_set(c->formats, key, APR_HASH_KEY_STRING, val);
    }

    // Read xcount and ycount
    c->xcount = c->ycount = -1;
    node = ezxml_child(c->node, "xcount");
    text = NULL;
    if (node) text = node->txt;
    if (text) c->xcount = (int)strtol(text, NULL, 10);
    node = ezxml_child(c->node, "ycount");
    text = NULL;
    if (node) text = node->txt;
    if (text) c->ycount = (int)strtol(text, NULL, 10);
  }


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve dimensions information
  //
  if (tileset->dimensions) {
    // Set up dimensions with default values
    dimensions = apr_array_make(ctx.pool, tileset->dimensions->nelts,
                                sizeof(mapcache_requested_dimension*));
    for ( i=0 ; i < tileset->dimensions->nelts ; i++ ) {
      mapcache_dimension * dim;
      mapcache_requested_dimension * reqdim;
      dim = APR_ARRAY_IDX(tileset->dimensions, i, mapcache_dimension*);
      reqdim = apr_pcalloc(ctx.pool, sizeof(mapcache_requested_dimension));
      reqdim->dimension = dim;
      reqdim->requested_value = dim->default_value;
      reqdim->cached_value = dim->default_value;
      APR_ARRAY_PUSH(dimensions, mapcache_requested_dimension*) = reqdim;
    }
    // Update dimensions with values specified with -D command line option
    // syntax is: "dim1=value1:dim2=value2:..."
    if (dim_spec) {
      char * ds = apr_pstrdup(ctx.pool, dim_spec);
      char *kvp, *last;
      for (kvp=apr_strtok(ds, ":", &last)
           ; kvp ; kvp = apr_strtok(NULL, ":", &last))
      {
        char *key, *val, *sav;
        key = apr_strtok(kvp, "=", &sav);
        val = apr_strtok(NULL, "=", &sav);
        if (!key || !val) {
          usage(ctx.pool, argv[0],
              "Can't parse dimension settings: %s", dim_spec);
          goto failure;
        }
        mapcache_set_requested_dimension(&ctx, dimensions, key, val);
        mapcache_set_cached_dimension(&ctx, dimensions, key, val);
        if (GC_HAS_ERROR(&ctx)) goto failure;
      }
    }
    // Check that dimension values are valid
    for ( i=0 ; i < dimensions->nelts ; i++) {
      mapcache_requested_dimension *entry;
      apr_array_header_t * vals;
      entry = APR_ARRAY_IDX(dimensions, i, mapcache_requested_dimension*);
      vals = mapcache_dimension_get_entries_for_value(&ctx,
                entry->dimension, entry->requested_value, tileset, NULL, grid);
      if (GC_HAS_ERROR(&ctx)) goto failure;
      if (!vals || vals->nelts == 0) {
        ctx.set_error(&ctx, 500,
            "invalid value \"%s\" for dimension \"%s\"\n",
            entry->requested_value, entry->dimension->name);
        goto failure;
      }
    }
  }


  /////////////////////////////////////////////////////////////////////////////
  // Set default query for counting tiles in a rectangular part of a SQLite
  // cache file
  //
  if (!count_query) {
    count_query = "SELECT count(rowid)"
                  "  FROM tiles"
                  " WHERE (x between :minx and :maxx)"
                  "   AND (y between :miny and :maxy)"
                  "   AND (z=:z)"
                  "   AND tileset=:tileset AND grid=:grid AND dim=:dim";
  }


  /////////////////////////////////////////////////////////////////////////////
  // Retrieve zoom level interval (defaults to 0)
  //
  if (zoom) {
    minzoom = strtol(zoom, &text, 10);
    maxzoom = minzoom;
    if (*text == ',') {
      maxzoom = strtol(text+1, &text, 10);
    }
    if (*text != '\0') {
      usage(ctx.pool, argv[0], "Bad int format for zoom level: %s", zoom);
      goto failure;
    }
    if (minzoom > maxzoom) {
      int swap = minzoom;
      minzoom = maxzoom;
      maxzoom = swap;
    }
    if (minzoom < 0) {
      ctx.set_error(&ctx, 500,
          "Zoom level %d not in valid interval [ %d, %d ]",
          minzoom, 0, grid->nlevels-1);
      goto failure;
    }
    if (maxzoom >= grid->nlevels) {
      ctx.set_error(&ctx, 500,
          "Zoom level %d not in valid interval [ %d, %d ]",
          maxzoom, 0, grid->nlevels-1);
      goto failure;
    }
  }


  /////////////////////////////////////////////////////////////////////////////
  // Report global identification information
  //
  if (json_output) {
    jreport = cJSON_CreateObject();
    cJSON_AddStringToObject(jreport, "layer", tileset->name);
    cJSON_AddStringToObject(jreport, "grid", grid->name);
    cJSON_AddStringToObject(jreport, "unit",
                            grid->unit==MAPCACHE_UNIT_METERS? "m":
                            grid->unit==MAPCACHE_UNIT_DEGREES?"dd":"ft");
    jregion = cJSON_CreateObject();
    cJSON_AddItemToObject(jreport, "region", jregion);
    jitem = cJSON_AddArrayToObject(jregion, "bounding_box");
    cJSON_AddNumberToObject(jitem, "", region_bbox.minx);
    cJSON_AddNumberToObject(jitem, "", region_bbox.miny);
    cJSON_AddNumberToObject(jitem, "", region_bbox.maxx);
    cJSON_AddNumberToObject(jitem, "", region_bbox.maxy);
#ifdef USE_CLIPPERS
    cJSON_AddItemToObject(jregion, "geometry",
        GEOSGeometry_to_cJSON(region_geom));
#else // Use region_bbox instead of region_geom
    cJSON_AddItemToObject(jregion, "geometry",
        mapcache_extent_to_cJSON(&region_bbox));
#endif // USE_CLIPPERS
    jzooms = cJSON_AddArrayToObject(jreport, "zoom_levels");
  }


  /////////////////////////////////////////////////////////////////////////////
  // Iterate over all tiles of all DB files of all requested zoom levels within
  // region of interest
  //


  // Iterate over all requested zoom levels
  for (iz = minzoom ; iz <= maxzoom ; iz ++) {
    int64_t tiles_max_in_level = 0;
    int64_t tiles_cached_in_level = 0;
    mapcache_extent_i til_region_bbox;
    mapcache_extent_i db_region_bbox;
    int dbx_has_inv = FALSE;
    int dby_has_inv = FALSE;

    // Select cache according to zoom level
    for ( cid=0 ; cid < caches->nelts ; cid++ ) {
      cache = APR_ARRAY_IDX(caches, cid, struct cache_info*);
      if ((iz >= cache->minzoom) && (iz <= cache->maxzoom)) break;
    }
    if (cache->cache->type != MAPCACHE_CACHE_SQLITE) {
      ctx.set_error(&ctx, 500,
          "cache \"%s\" of tileset \"%s\" is not of type SQLite",
          cache->cache->name, tileset->name);
      goto failure;
    }

    // Report identification information for current zoom level
    if (json_output) {
      jzoom = cJSON_CreateObject();
      cJSON_AddItemToObject(jzooms, "", jzoom);
      cJSON_AddNumberToObject(jzoom, "level", iz);
      jfiles = cJSON_AddArrayToObject(jzoom, "files");
    }

    // Compute region bounding box expressed in tiles and in DB files for the
    // current zoom level
    mapcache_grid_get_xy(&ctx, grid, region_bbox.minx, region_bbox.miny, iz,
        &(til_region_bbox.minx), &(til_region_bbox.miny));
    mapcache_grid_get_xy(&ctx, grid, region_bbox.maxx, region_bbox.maxy, iz,
        &(til_region_bbox.maxx), &(til_region_bbox.maxy));

    dbx_has_inv = strstr(cache->dbfile,"{inv_x}")
      || strstr(cache->dbfile,"{inv_div_x}");
    dby_has_inv = strstr(cache->dbfile,"{inv_y}")
      || strstr(cache->dbfile,"{inv_div_y}");
    if ((cache->xcount > 0) && (cache->ycount > 0)) {
      if (dbx_has_inv) {
        int inv_minx = grid->levels[iz]->maxx - til_region_bbox.minx;
        int inv_maxx = grid->levels[iz]->maxx - til_region_bbox.maxx;
        db_region_bbox.minx = floor(inv_maxx/cache->ycount);
        db_region_bbox.maxx = floor(inv_minx/cache->ycount);
      } else {
        db_region_bbox.minx = floor(til_region_bbox.minx/cache->xcount);
        db_region_bbox.maxx = floor(til_region_bbox.maxx/cache->xcount);
      }
      if (dby_has_inv) {
        int inv_miny = grid->levels[iz]->maxy - til_region_bbox.miny;
        int inv_maxy = grid->levels[iz]->maxy - til_region_bbox.maxy;
        db_region_bbox.miny = floor(inv_maxy/cache->ycount);
        db_region_bbox.maxy = floor(inv_miny/cache->ycount);
      } else {
        db_region_bbox.miny = floor(til_region_bbox.miny/cache->ycount);
        db_region_bbox.maxy = floor(til_region_bbox.maxy/cache->ycount);
      }
    } else {
      db_region_bbox.minx = 0;
      db_region_bbox.miny = 0;
      db_region_bbox.maxx = 0;
      db_region_bbox.maxy = 0;
    }

    // Iterate over DB files of current level within region bounding box
    if (!db_files) db_files = apr_hash_make(ctx.pool);
    for (ix = db_region_bbox.minx ; ix <= db_region_bbox.maxx ; ix++) {
      for (iy = db_region_bbox.miny ; iy <= db_region_bbox.maxy ; iy++) {
        int tiles_max_in_file = 0;
        int tiles_cached_in_file = 0;
        char * file_name;
        apr_status_t file_open_report;
        apr_file_t * filehandle;
        apr_finfo_t fileinfo;
        mapcache_extent_i til_file_bbox;
        mapcache_extent file_bbox;
        mapcache_extent temp_bbox;
        mapcache_extent region_in_file_bbox;
#ifdef USE_CLIPPERS
        GEOSGeometry *file_geom;
        GEOSGeometry *temp_geom;
        GEOSGeometry *region_in_file_geom;
        const GEOSGeometry *temp_ring;
        int npoints, p;
#endif // USE_CLIPPERS
        mapcache_extent_i til_region_in_file_bbox;
        int region_in_file_is_rectangle = FALSE;
        int region_in_file_is_empty = FALSE;
        apr_off_t *sizeref;
        double res;

        // Display progression
        if (show_progress) {
          double incz = 1.0 / (double)(maxzoom - minzoom + 1);
          double incx = incz / (double)(db_region_bbox.maxx
                                        - db_region_bbox.minx + 1);
          double incy = incx / (double)(db_region_bbox.maxy
                                        - db_region_bbox.miny + 1);
          double progz = (double)(iz - minzoom) * incz;
          double progx = (double)(ix - db_region_bbox.minx) * incx;
          double progy = (double)(iy - db_region_bbox.miny) * incy;
          fprintf(stderr, "\033[2K In progress (z:%d x:%d y:%d): %.3f%% done\r",
              iz, ix, iy, (progz + progx + progy) * 100.0);
          fflush(stderr);
        }

        // Retrieve DB file name and check for its existence (read access)
        file_name = dbfilename(ctx.pool, cache->dbfile, tileset, grid,
            dimensions, cache->formats, iz, ix, iy, cache->xcount,
            cache->ycount);

        // Unless this has already been done on this file,
        // Retrieve file size and count cached tiles regardless the region of
        // interest (for average tile size estimation)
        fileinfo.size = 0;
        sizeref = apr_hash_get(db_files, file_name, APR_HASH_KEY_STRING);
        if (sizeref) {
          fileinfo.size = *sizeref;
        } else {
          file_open_report = apr_file_open(&filehandle, file_name,
              APR_FOPEN_READ|APR_FOPEN_BINARY, APR_FPROT_OS_DEFAULT, ctx.pool);
          if (file_open_report == APR_SUCCESS) {
            int level;
            // Retrieve file size
            apr_file_info_get(&fileinfo, APR_FINFO_SIZE, filehandle);
            apr_file_close(filehandle);
            sizeref = apr_pcalloc(ctx.pool, sizeof(apr_off_t));
            *sizeref = fileinfo.size;
            apr_hash_set(db_files, file_name, APR_HASH_KEY_STRING, sizeref);
            size_of_cache += fileinfo.size;
            // Retrieve total number of cached tiles in file
            for (level = 0 ; level < grid->nlevels ; level++) {
              int tmpmax, tmpcached;
              const mapcache_extent_i full_extent = { 0, 0, INT_MAX, INT_MAX };
              count_tiles_in_rectangle(&ctx, level, full_extent, tileset,
                  grid_link, dimensions, file_name, count_query, &tmpmax,
                  &tmpcached);
              if (GC_HAS_ERROR(&ctx)) goto failure;
              tiles_in_cache += tmpcached;
            }
          }
          else
          {
            nb_missing_files++ ;
          }
        }

        // Compute file bounding box expressed in tiles
        if ((cache->xcount > 0) && (cache->ycount > 0)) {
          if (dbx_has_inv) {
            til_file_bbox.maxx = grid->levels[iz]->maxx-1 - ix * cache->xcount;
            til_file_bbox.minx = til_file_bbox.maxx + cache->xcount + 1;
          } else {
            til_file_bbox.minx = ix * cache->xcount;
            til_file_bbox.maxx = til_file_bbox.minx + cache->xcount - 1;
          }
          if (dby_has_inv) {
            til_file_bbox.maxy = grid->levels[iz]->maxy-1 - iy * cache->ycount;
            til_file_bbox.miny = til_file_bbox.maxy - cache->ycount + 1;
          } else {
            til_file_bbox.miny = iy * cache->ycount;
            til_file_bbox.maxy = til_file_bbox.miny + cache->ycount - 1;
          }
          if (til_file_bbox.minx < 0) til_file_bbox.minx = 0;
          if (til_file_bbox.miny < 0) til_file_bbox.miny = 0;
          if (til_file_bbox.maxx >= grid->levels[iz]->maxx) {
            til_file_bbox.maxx = grid->levels[iz]->maxx - 1;
          }
          if (til_file_bbox.maxy >= grid->levels[iz]->maxy) {
            til_file_bbox.maxy = grid->levels[iz]->maxy - 1;
          }
        } else {
          til_file_bbox.minx = 0;
          til_file_bbox.miny = 0;
          til_file_bbox.maxx = grid->levels[iz]->maxx - 1;
          til_file_bbox.maxy = grid->levels[iz]->maxy - 1;
        }

        // Compute file bounding box expressed in grid units for the current
        // zoom level
        mapcache_grid_get_tile_extent(&ctx, grid, til_file_bbox.minx,
            til_file_bbox.miny, iz, &temp_bbox);
        if (GC_HAS_ERROR(&ctx)) goto failure;
        file_bbox.minx = temp_bbox.minx;
        file_bbox.miny = temp_bbox.miny;
        mapcache_grid_get_tile_extent(&ctx, grid, til_file_bbox.maxx,
            til_file_bbox.maxy, iz, &temp_bbox);
        if (GC_HAS_ERROR(&ctx)) goto failure;
        file_bbox.maxx = temp_bbox.maxx;
        file_bbox.maxy = temp_bbox.maxy;

        // Compute part of region of interest within file bounding box
#ifdef USE_CLIPPERS
        file_geom = mapcache_extent_to_GEOSGeometry(&file_bbox);
        region_in_file_geom = GEOSIntersection(region_geom, file_geom);
        region_in_file_is_empty = GEOSisEmpty(region_in_file_geom);
#else // use <var>_bbox instead of <var>_geom
        region_in_file_is_rectangle = TRUE;
        region_in_file_bbox.minx = fmaxl(region_bbox.minx, file_bbox.minx);
        region_in_file_bbox.maxx = fminl(region_bbox.maxx, file_bbox.maxx);
        if (region_in_file_bbox.minx > region_in_file_bbox.maxx) {
          region_in_file_bbox.minx = region_in_file_bbox.maxx = 0;
          region_in_file_is_empty = TRUE;
        }
        region_in_file_bbox.miny = fmaxl(region_bbox.miny, file_bbox.miny);
        region_in_file_bbox.maxy = fminl(region_bbox.maxy, file_bbox.maxy);
        if (region_in_file_bbox.miny > region_in_file_bbox.maxy) {
          region_in_file_bbox.miny = region_in_file_bbox.maxy = 0;
          region_in_file_is_empty = TRUE;
        }
#endif // USE_CLIPPERS

        // Jump to next file if this one is outside the region of interest
        if (region_in_file_is_empty) continue;

#ifdef USE_CLIPPERS
        // Compute bounding box of region/file intersection expressed in grid
        // units
        temp_geom = GEOSEnvelope(region_in_file_geom);
        temp_ring = GEOSGetExteriorRing(temp_geom);
        npoints = GEOSGeomGetNumPoints(temp_ring);
        for (p=0 ; p<npoints ; p++) {
          double px, py;
          const GEOSCoordSequence * coordseq;
          coordseq = GEOSGeom_getCoordSeq(temp_ring);
          GEOSCoordSeq_getX(coordseq, p, &px);
          GEOSCoordSeq_getY(coordseq, p, &py);
          if (p==0) {
            region_in_file_bbox.minx = px;
            region_in_file_bbox.miny = py;
            region_in_file_bbox.maxx = px;
            region_in_file_bbox.maxy = py;
          } else {
            region_in_file_bbox.minx = fminl(region_in_file_bbox.minx, px);
            region_in_file_bbox.miny = fminl(region_in_file_bbox.miny, py);
            region_in_file_bbox.maxx = fmaxl(region_in_file_bbox.maxx, px);
            region_in_file_bbox.maxy = fmaxl(region_in_file_bbox.maxy, py);
          }
        }

        // Check whether region/file intersection is a rectangle
        region_in_file_is_rectangle =
          (GEOSEquals(region_in_file_geom, temp_geom) == 1);
        GEOSGeom_destroy(temp_geom);
#endif // USE_CLIPPERS

        // Compute bounding box of region/file intersection expressed in
        // tiles for current zoom level
        mapcache_grid_get_xy(&ctx, grid, region_in_file_bbox.minx,
            region_in_file_bbox.miny, iz, &(til_region_in_file_bbox.minx),
            &(til_region_in_file_bbox.miny));
        // Note: upper endpoints maxx and maxy do not belong to the bbox
        res = grid->levels[iz]->resolution;
        mapcache_grid_get_xy(&ctx, grid, region_in_file_bbox.maxx-res,
            region_in_file_bbox.maxy-res, iz, &(til_region_in_file_bbox.maxx),
            &(til_region_in_file_bbox.maxy));
        if ((cache->xcount > 0) && (cache->ycount > 0)) {
          if (til_region_in_file_bbox.maxx>(til_file_bbox.minx+cache->xcount-1))
          {
            (til_region_in_file_bbox.maxx)--;
          }
          if (til_region_in_file_bbox.maxy>(til_file_bbox.miny+cache->ycount-1))
          {
            (til_region_in_file_bbox.maxy)--;
          }
        }

        // If region/file intersection is a rectangle, count tiles in a single
        // step
        if (region_in_file_is_rectangle) {
          int tmpmax, tmpcached;
          count_tiles_in_rectangle(
              &ctx,
              iz,
              til_region_in_file_bbox,
              tileset,
              grid_link,
              dimensions,
              file_name,
              count_query,
              &tmpmax,
              &tmpcached
              );
          tiles_max_in_file += tmpmax;
          tiles_cached_in_file += tmpcached;
        }

#ifdef USE_CLIPPERS
        // Else, if file is partially inside region, iterate over tiles
        else {
          int tmpmax, tmpcached;
          count_tiles_in_region(
              &ctx,
              iz,
              til_region_in_file_bbox,
              tileset,
              grid_link,
              dimensions,
              region_prepgeom,
              (file_open_report==APR_SUCCESS),
              &tmpmax,
              &tmpcached
              );
          tiles_max_in_file += tmpmax;
          tiles_cached_in_file += tmpcached;
        }
#endif // USE_CLIPPERS

        // Report identification and coverage information for a single DB file
        // for current zoom level and extent
        if (json_output) {
          if (report_missing_files || (file_open_report == APR_SUCCESS)) {
            jfile = cJSON_CreateObject();
            cJSON_AddItemToObject(jfiles, "", jfile);
            cJSON_AddStringToObject(jfile, "file_name", file_name);
            cJSON_AddNumberToObject(jfile, "file_size", fileinfo.size);
            jitem = cJSON_AddArrayToObject(jfile, "file_bounding_box");
            cJSON_AddNumberToObject(jitem, "", file_bbox.minx);
            cJSON_AddNumberToObject(jitem, "", file_bbox.miny);
            cJSON_AddNumberToObject(jitem, "", file_bbox.maxx);
            cJSON_AddNumberToObject(jitem, "", file_bbox.maxy);
            jregion = cJSON_CreateObject();
            cJSON_AddItemToObject(jfile, "region_in_file", jregion);
            jitem = cJSON_AddArrayToObject(jregion, "bounding_box");
            cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.minx);
            cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.miny);
            cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.maxx);
            cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.maxy);
#ifdef USE_CLIPPERS
            jitem = GEOSGeometry_to_cJSON(region_in_file_geom);
#else // use <var>_bbox instead of <var>_geom
            jitem = mapcache_extent_to_cJSON(&region_in_file_bbox);
#endif // USE_CLIPPERS
            cJSON_AddItemToObject(jregion, "geometry", jitem);
            jitem = cJSON_CreateObject();
            cJSON_AddItemToObject(jfile, "nb_tiles_in_region", jitem);
            cJSON_AddNumberToObject(jitem,"missing_in_file",
                tiles_max_in_file - tiles_cached_in_file);
            cJSON_AddNumberToObject(jitem,"cached_in_file",
                tiles_cached_in_file);
            cJSON_AddNumberToObject(jitem,"max_in_file", tiles_max_in_file);
            cJSON_AddNumberToObject(jitem,"coverage",
                (double)tiles_cached_in_file/(double)tiles_max_in_file);
          }
        }

        // Add tiles in file to tiles in zoom level
        tiles_max_in_level += tiles_max_in_file;
        tiles_cached_in_level += tiles_cached_in_file;

        // Release resources that are no longer needed
#ifdef USE_CLIPPERS
        GEOSGeom_destroy(file_geom);
        GEOSGeom_destroy(region_in_file_geom);
#endif // USE_CLIPPERS
      }
    }

    // Add tiles in level to tiles in cache
    tiles_max_in_cache += tiles_max_in_level;
    tiles_cached_in_cache += tiles_cached_in_level;

    // Report coverage information for current zoom level and extent
    if (json_output) {
      jitem = cJSON_CreateObject();
      cJSON_AddItemToObject(jzoom, "nb_tiles_in_region", jitem);
      cJSON_AddNumberToObject(jitem, "missing_in_level",
          tiles_max_in_level - tiles_cached_in_level);
      cJSON_AddNumberToObject(jitem, "cached_in_level", tiles_cached_in_level);
      cJSON_AddNumberToObject(jitem, "max_in_level", tiles_max_in_level);
      cJSON_AddNumberToObject(jitem,"coverage",
          (double)tiles_cached_in_level/(double)tiles_max_in_level);
    }
  }


  // Report global coverage information
  if (json_output) {
    jitem = cJSON_CreateObject();
    cJSON_AddItemToObject(jreport, "nb_tiles_in_region", jitem);
    cJSON_AddNumberToObject(jitem, "missing_in_cache",
        tiles_max_in_cache - tiles_cached_in_cache);
    cJSON_AddNumberToObject(jitem, "cached_in_cache", tiles_cached_in_cache);
    cJSON_AddNumberToObject(jitem, "max_in_cache", tiles_max_in_cache);
    cJSON_AddNumberToObject(jitem,"coverage",
        (double)tiles_cached_in_cache/(double)tiles_max_in_cache);
    jitem = cJSON_CreateObject();
    cJSON_AddItemToObject(jreport, "sizes", jitem);
    cJSON_AddNumberToObject(jitem, "total_size_of_files", size_of_cache);
    cJSON_AddNumberToObject(jitem, "total_nbtiles_in_files", tiles_in_cache);
    if (tiles_in_cache > 0) {
      apr_off_t average_tile_size = size_of_cache/tiles_in_cache;

      cJSON_AddNumberToObject(jitem, "average_tile_size", average_tile_size);
      cJSON_AddNumberToObject(jitem, "estimated_max_cache_size",
          average_tile_size*tiles_max_in_cache);
      cJSON_AddNumberToObject(jitem, "estimated_cached_cache_size",
          average_tile_size*tiles_cached_in_cache);
      cJSON_AddNumberToObject(jitem, "estimated_missing_cache_size",
          average_tile_size*(tiles_max_in_cache-tiles_cached_in_cache));
    } else {
      const char * na = "N/A: cache is empty";

      cJSON_AddStringToObject(jitem, "average_tile_size", na);
      cJSON_AddStringToObject(jitem, "estimated_max_cache_size", na);
      cJSON_AddNumberToObject(jitem, "estimated_cached_cache_size", 0);
      cJSON_AddStringToObject(jitem, "estimated_missing_cache_size", na);
    }
    if (!report_missing_files) {
      cJSON_AddNumberToObject(jreport, "nb_missing_files", nb_missing_files);
    }
  }

  // Last progression indication
  if (show_progress) {
    fprintf(stderr, "\033[2K Finished: 100.000%% done\n");
  }

  // Display JSON report
  if (json_output) {
    printf("%s\n", cJSON_Print(jreport));
  }


success:
  _destroy_json_pool();
#ifdef USE_CLIPPERS
  finishGEOS();
#endif // USE_CLIPPERS
  apr_terminate();
  return 0;


failure:
  if (GC_HAS_ERROR(&ctx)) {
    fprintf(stderr, "%s: %s\n", base_name(argv[0]),
        ctx.get_error_message(&ctx));
  }
  _destroy_json_pool();
#ifdef USE_CLIPPERS
  finishGEOS();
#endif // USE_CLIPPERS
  apr_terminate();
  return 1;
}


// vim: set tw=79 ts=2 sw=2 sts=2 et ai si nonu syn=c fo+=ro :
