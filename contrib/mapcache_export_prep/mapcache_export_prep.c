#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <apr_general.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <sqlite3.h>
#include "mapcache.h"
#include "ezxml.h"
#include "cJSON.h"
#include <geos_c.h>
#include <ogr_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>


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
  { "ogr-datasource", 'd', TRUE,  "OGR data source to get features from."
                                    " Cannot be used with --extent." },
  { "ogr-layer",      'l', TRUE,  "OGR layer inside OGR data source. Cannot"
                                    " be used with --ogr-sql." },
  { "ogr-where",      'w', TRUE,  "Filter to apply on OGR layer features."
                                    " Cannot be used with --ogr-sql." },
  { "ogr-sql",        's', TRUE,  "SQL query to filter inside OGR data"
                                    " source. Cannot be used with"
                                    " --ogr-layer or --ogr-where." },
  { "zoom",           'z', TRUE,  "Set min and max zoom levels to analyze,"
                                    " separated by a comma, eg: 12,15" },
  { "query",          'q', TRUE,  "Set query for counting tiles in a"
                                    " rectangle. Default value works with"
                                    " default schema of SQLite caches." },
  { "endofopt",        0,  FALSE, "End of options" }
};

// Extract basename from a path
const char * base_name(const char * path)
{
  const char *subdir, *basename;
  for (subdir=path ; (subdir=strchr(subdir, '/')) ; basename=++subdir);
  return basename;
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
    struct winsize w;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) < 0) w.ws_col = 0;
    fprintf(stderr, "    -%c | --%s%s\n                ",
        optlist[i].optch, optlist[i].name,
        optlist[i].has_arg ? " <value>" : "");
    apr_tokenize_to_argv(optlist[i].description, &words, pool);
    for (;*words;words++) {
      linewidth += strlen(*words)+1;
      if (w.ws_col > 0 && linewidth > w.ws_col) {
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
                  int tilx, int tily, int xcount, int ycount)
{
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
    int maxx = grid->levels[z]->maxx;
    char * curfmt;
    curfmt = apr_hash_get(fmt, "x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{x}",
        apr_psprintf(pool, curfmt, tilx/xcount*xcount));
    curfmt = apr_hash_get(fmt, "div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_x}",
        apr_psprintf(pool, curfmt, tilx/xcount));
    curfmt = apr_hash_get(fmt, "inv_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_x}",
        apr_psprintf(pool, curfmt, (maxx-1-tilx)/xcount*xcount));
    curfmt = apr_hash_get(fmt, "inv_div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_x}",
        apr_psprintf(pool, curfmt, (maxx-1-tilx)/xcount));
  }

  // Y coordinate
  if (ycount > 0) {
    int maxy = grid->levels[z]->maxy;
    char * curfmt;
    curfmt = apr_hash_get(fmt, "y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{y}",
        apr_psprintf(pool, curfmt, tily/ycount*ycount));
    curfmt = apr_hash_get(fmt, "div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_y}",
        apr_psprintf(pool, curfmt, tily/ycount));
    curfmt = apr_hash_get(fmt, "inv_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_y}",
        apr_psprintf(pool, curfmt, (maxy-1-tily)/ycount*ycount));
    curfmt = apr_hash_get(fmt, "inv_div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_y}",
        apr_psprintf(pool, curfmt, (maxy-1-tily)/ycount));
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
    ctx->set_error(ctx, 500, "SQLite failed: '%s'", sqlite3_errmsg(db));
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
      ctx->set_error(ctx, 500, "SQLite returned no tile count");
      count = 0;
      break;
    default:
      ctx->set_error(ctx, 500, "SQLite failed: '%s'", sqlite3_errmsg(db));
      count = 0;
  }

  sqlite3_finalize(res);
  sqlite3_close(db);

  *tcached = count;
}


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
  GEOSGeometry *region_geom;
  GEOSGeometry *grid_geom;
  const GEOSPreparedGeometry *region_prepgeom = NULL;
  mapcache_tileset * tileset = NULL;
  mapcache_grid * grid = NULL;
  mapcache_grid_link * grid_link;
  mapcache_cache * cache;
  apr_array_header_t * dimensions = NULL;
  char * cache_dbfile = NULL;
  apr_hash_t * xyz_fmt;
  int i, ix, iy, iz;
  ezxml_t doc, node;
  ezxml_t cache_node = NULL;
  ezxml_t dbfile_node = NULL;
  char * text;
  apr_hash_index_t * hi;
  int cache_xcount, cache_ycount;
  mapcache_extent region_bbox = { 0, 0, 0, 0 };
  double * list = NULL;
  int nelts;
  int minzoom = 0, maxzoom = 0;
  double coverage;
  int64_t tiles_max_in_cache = 0;
  int64_t tiles_cached_in_cache = 0;
  double cache_max = 0, cache_cached = 0;
  apr_off_t cache_size = 0;


  /////////////////////////////////////////////////////////////////////////////
  // Initialize Apache, GEOS, OGR, cJSON and Mapcache
  //
  apr_initialize();
  initGEOS(notice, log_and_exit);
  OGRRegisterAll();
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
  if (!extent && !ogr_file) {
    ctx.set_error(&ctx, 500,
        "Neither Extent nor OGR Data Source has been specified");
    goto failure;
  }

  // Region of interest is specified with --extent
  if (extent) {
    GEOSGeometry *temp;
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
    temp = mapcache_extent_to_GEOSGeometry(&region_bbox);
    region_geom = temp;
  }

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

  // Region of interest must be within grid extent
  region_prepgeom = GEOSPrepare(region_geom);
  grid_geom = mapcache_extent_to_GEOSGeometry(&(grid->extent));
  if (GEOSPreparedWithin(region_prepgeom, grid_geom) != 1) {
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
  cache = tileset->_cache;
  if (cache->type != MAPCACHE_CACHE_SQLITE) {
    ctx.set_error(&ctx, 500,
        "cache \"%s\" of tileset \"%s\" is not of type SQLite",
        cache->name, tileset->name);
    goto failure;
  }
  for (node = ezxml_child(doc, "cache") ; node ; node = node->next) {
    if (strcmp(ezxml_attr(node, "name"), cache->name) == 0) {
      cache_node = node;
      break;
    }
  }
  if (!cache_node) {
    ctx.set_error(&ctx, 500,
        "cache \"%s\" has not been not found", cache->name);
    goto failure;
  }
  dbfile_node = ezxml_child(cache_node, "dbfile");
  cache_dbfile = dbfile_node->txt;
  if (!cache_dbfile) {
    ctx.set_error(&ctx, 500,
        "Failed to parse <dbfile> tag of cache \"%s\"", cache->name);
    goto failure;
  }
  xyz_fmt = apr_hash_make(ctx.pool);
  apr_hash_set(xyz_fmt, "x",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "y",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "z",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_x",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_y",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "div_x",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "div_y",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_div_x", APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_div_y", APR_HASH_KEY_STRING, "(not set)");
  for (hi = apr_hash_first(ctx.pool, xyz_fmt) ; hi ; hi = apr_hash_next(hi)) {
    const char *key, *val, *attr;
    apr_hash_this(hi, (const void**)&key, NULL, (void**)&val);
    attr = apr_pstrcat(ctx.pool, key, "_fmt", NULL);
    val = ezxml_attr(dbfile_node, attr);
    if (!val) val = "%d";
    apr_hash_set(xyz_fmt, key, APR_HASH_KEY_STRING, val);
  }
  cache_xcount = cache_ycount = -1;
  text = ezxml_child(cache_node, "xcount")->txt;
  if (text) cache_xcount = (int)strtol(text, NULL, 10);
  text = ezxml_child(cache_node, "ycount")->txt;
  if (text) cache_ycount = (int)strtol(text, NULL, 10);


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
      vals = entry->dimension->get_entries_for_value(&ctx,
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
    cJSON_AddItemToObject(jregion, "geometry",
                          GEOSGeometry_to_cJSON(region_geom));
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
    //--
    double zoom_max = 0, zoom_cached = 0;

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
    db_region_bbox.minx = floor(til_region_bbox.minx/cache_xcount);
    db_region_bbox.miny = floor(til_region_bbox.miny/cache_ycount);
    db_region_bbox.maxx = floor(til_region_bbox.maxx/cache_xcount);
    db_region_bbox.maxy = floor(til_region_bbox.maxy/cache_ycount);

    // Iterate over DB files of current level within region bounding box
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
        GEOSGeometry *file_geom;
        mapcache_extent region_in_file_bbox;
        GEOSGeometry *region_in_file_geom;
        GEOSGeometry *temp_geom;
        const GEOSGeometry *temp_ring;
        int npoints, p;
        mapcache_extent_i til_region_in_file_bbox;
        int region_in_file_is_rectangle = FALSE;
        //--
        int x = ix * cache_xcount;
        int y = iy * cache_ycount;
///        int nbtiles_file_cached;
///        int nbtiles_extent_max, nbtiles_extent_cached;
///        mapcache_extent area;
///        const mapcache_extent full_extent = {
///              0, 0, (double)INT_MAX, (double)INT_MAX };

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
          fprintf(stderr, " In progress: %.3f%% done (%d,%d,%d)\r",
                          (progz + progx + progy) * 100.0,iz,ix,iy);
          fflush(stderr);
        }

        // Retrieve DB file name and check for its existence (read access)
        file_name = dbfilename(ctx.pool, cache_dbfile, tileset, grid,
            dimensions, xyz_fmt, iz, x, y, cache_xcount, cache_ycount);
        file_open_report = apr_file_open(&filehandle, file_name,
              APR_FOPEN_READ|APR_FOPEN_BINARY, APR_FPROT_OS_DEFAULT, ctx.pool);

        // Retrieve file size
        fileinfo.size = 0;
        if (file_open_report == APR_SUCCESS) {
          apr_file_info_get(&fileinfo, APR_FINFO_SIZE, filehandle);
          apr_file_close(filehandle);
        }

        // Compute file bounding box expressed in tiles
        til_file_bbox.minx = ix * cache_xcount;
        til_file_bbox.miny = iy * cache_ycount;
        til_file_bbox.maxx = til_file_bbox.minx + cache_xcount - 1;
        til_file_bbox.maxy = til_file_bbox.miny + cache_ycount - 1;

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
        file_geom = mapcache_extent_to_GEOSGeometry(&file_bbox);
        region_in_file_geom = GEOSIntersection(region_geom, file_geom);

        // Jump to next file if this one is outside the region of interest
        if (GEOSisEmpty(region_in_file_geom)) continue;

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

        // Compute bounding box of region/file intersection expressed in
        // tiles for current zoom level
        mapcache_grid_get_xy(&ctx, grid, region_in_file_bbox.minx,
            region_in_file_bbox.miny, iz, &(til_region_in_file_bbox.minx),
            &(til_region_in_file_bbox.miny));
        mapcache_grid_get_xy(&ctx, grid, region_in_file_bbox.maxx,
            region_in_file_bbox.maxy, iz, &(til_region_in_file_bbox.maxx),
            &(til_region_in_file_bbox.maxy));
        if (til_region_in_file_bbox.maxx > (x+cache_xcount-1)) {
          (til_region_in_file_bbox.maxx)--;
        }
        if (til_region_in_file_bbox.maxy > (y+cache_ycount-1)) {
          (til_region_in_file_bbox.maxy)--;
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

          // Build up DB file name
///       dbfile = dbfilename(ctx.pool, cache_dbfile, tileset, grid, dimensions,
///           xyz_fmt, iz, x, y, cache_xcount, cache_ycount);

///       fileinfo.size = 0;
///        nbtiles_file_max = cache_xcount * cache_ycount;
///        nbtiles_file_cached = 0;
///        area.minx = fmaxl(x, til.minx);
///        area.miny = fmaxl(y, til.miny);
///        area.maxx = fminl(x+cache_xcount-1, til.maxx);
///        area.maxy = fminl(y+cache_ycount-1, til.maxy);
///        nbtiles_extent_max = (area.maxx-area.minx+1)*(area.maxy-area.miny+1);
///        nbtiles_extent_cached = 0;

///       // If file exists, get its size and its cached tile counts both in
///       // total and in specified extent
///       open_dbfile_report = apr_file_open(&filehandle, dbfile,
///           APR_FOPEN_READ|APR_FOPEN_BINARY, APR_FPROT_OS_DEFAULT, ctx.pool);
///       if (open_dbfile_report == APR_SUCCESS) {
///         // Get file size
///         apr_file_info_get(&fileinfo, APR_FINFO_SIZE, filehandle);
///         apr_file_close(filehandle);

///          // Get number of cached tiles within extent present in file
///          nbtiles_extent_cached = count_tiles(&ctx, dbfile, count_query, til,
///                                              iz, grid, tileset, dimensions);
///
///          // Get total number of cached tiles present in file
///          nbtiles_file_cached = count_tiles(&ctx, dbfile, count_query,
///                                            full_extent, iz, grid, tileset,
///                                            dimensions);
///       }
///        zoom_max += nbtiles_extent_max;
///        zoom_cached += nbtiles_extent_cached;
///        cache_max += nbtiles_extent_max;
///        cache_cached += nbtiles_extent_cached;
///        cache_size += fileinfo.size;

///     // Compute extent of region inside current DB file
///     reg_in_db_bbox = GEOSEnvelope(region_in_file_geom);
///     reg_in_db_ring = GEOSGetExteriorRing(reg_in_db_bbox);
///     npoints = GEOSGeomGetNumPoints(reg_in_db_ring);
///     for (p=0 ; p<npoints ; p++) {
///       double xcoord, ycoord;
///       const GEOSCoordSequence * coordseq;
///       coordseq = GEOSGeom_getCoordSeq(reg_in_db_ring);
///       GEOSCoordSeq_getX(coordseq, p, &xcoord);
///       GEOSCoordSeq_getY(coordseq, p, &ycoord);
///       if (p==0) {
///         reg_in_db_extent.minx = xcoord;
///         reg_in_db_extent.miny = ycoord;
///         reg_in_db_extent.maxx = xcoord;
///         reg_in_db_extent.maxy = ycoord;
///       } else {
///         reg_in_db_extent.minx = fminl(reg_in_db_extent.minx, xcoord);
///         reg_in_db_extent.miny = fminl(reg_in_db_extent.miny, ycoord);
///         reg_in_db_extent.maxx = fmaxl(reg_in_db_extent.maxx, xcoord);
///         reg_in_db_extent.maxy = fmaxl(reg_in_db_extent.maxy, ycoord);
///       }
///     }
///     GEOSGeom_destroy(reg_in_db_bbox);
///     mapcache_grid_get_xy(&ctx, grid,
///         reg_in_db_extent.minx, reg_in_db_extent.miny, iz,
///         &(til_reg_in_db.minx), &(til_reg_in_db.miny));
///     mapcache_grid_get_xy(&ctx, grid,
///         reg_in_db_extent.maxx, reg_in_db_extent.maxy, iz,
///         &(til_reg_in_db.maxx), &(til_reg_in_db.maxy));
///     if (til_reg_in_db.maxx > (x+cache_xcount-1)) (til_reg_in_db.maxx)--;
///     if (til_reg_in_db.maxy > (y+cache_ycount-1)) (til_reg_in_db.maxy)--;

///     if (GEOSPreparedContains(region_prepgeom, file_geom)) {
///       printf("DEBUG: %d:(%d,%d)\n", iz,ix,iy);
///     } else
///     {
///       // Iterate over tiles in DB file belonging to region of interest
///       for (tx = til_region_in_file_bbox.minx ;
///            tx <= til_region_in_file_bbox.maxx ; tx++) {
///         for (ty = til_region_in_file_bbox.miny ;
///              ty <= til_region_in_file_bbox.maxy ; ty++) {
///           mapcache_extent tilextent;

///           mapcache_grid_get_tile_extent(&ctx,grid,tx,ty,iz,&tilextent);
///           if (!tilgeom) {
///             tilgeom = mapcache_extent_to_GEOSGeometry(&tilextent);
///           } else {
///             update_GEOSGeometry_with_mapcache_extent(tilgeom, &tilextent);
///           }
///           if (GEOSPreparedIntersects(region_prepgeom, tilgeom) == 1) {
///             tiles_max_in_file++;
///           }
///         }
///       }
///     }

        // Report identification and coverage information for a single DB file
        // for current zoom level and extent
        if (json_output) {
///          coverage = (double)nbtiles_extent_cached/(double)nbtiles_extent_max;
          jfile = cJSON_CreateObject();
          cJSON_AddItemToObject(jfiles, "", jfile);
          cJSON_AddStringToObject(jfile, "file_name", file_name);
          cJSON_AddNumberToObject(jfile, "file_size", fileinfo.size);
          jitem = cJSON_AddArrayToObject(jfile, "file_bounding_box");
          cJSON_AddNumberToObject(jitem, "", file_bbox.minx);
          cJSON_AddNumberToObject(jitem, "", file_bbox.miny);
          cJSON_AddNumberToObject(jitem, "", file_bbox.maxx);
          cJSON_AddNumberToObject(jitem, "", file_bbox.maxy);
///       jitem = GEOSGeometry_to_cJSON(reg_in_db_bbox);
///       cJSON_AddItemToObject(jfile, "region_bounding_box", jitem);
///       jitem = cJSON_AddArrayToObject(jfile, "region_extent");
///       cJSON_AddNumberToObject(jitem, "", reg_in_db_extent.minx);
///       cJSON_AddNumberToObject(jitem, "", reg_in_db_extent.miny);
///       cJSON_AddNumberToObject(jitem, "", reg_in_db_extent.maxx);
///       cJSON_AddNumberToObject(jitem, "", reg_in_db_extent.maxy);
///       jitem = cJSON_AddArrayToObject(jfile, "til_region_extent");
///       cJSON_AddNumberToObject(jitem, "", til_reg_in_db.minx);
///       cJSON_AddNumberToObject(jitem, "", til_reg_in_db.miny);
///       cJSON_AddNumberToObject(jitem, "", til_reg_in_db.maxx);
///       cJSON_AddNumberToObject(jitem, "", til_reg_in_db.maxy);
          jregion = cJSON_CreateObject();
          cJSON_AddItemToObject(jfile, "region_in_file", jregion);
          jitem = cJSON_AddArrayToObject(jregion, "bounding_box");
          cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.minx);
          cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.miny);
          cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.maxx);
          cJSON_AddNumberToObject(jitem, "", region_in_file_bbox.maxy);
          jitem = GEOSGeometry_to_cJSON(region_in_file_geom);
          cJSON_AddItemToObject(jregion, "geometry", jitem);
          jitem = cJSON_CreateObject();
          cJSON_AddNumberToObject(jitem,"max_in_file", tiles_max_in_file);
          cJSON_AddNumberToObject(jitem,"cached_in_file", tiles_cached_in_file);
///          cJSON_AddNumberToObject(jitem,"file_cached",nbtiles_file_cached);
///          cJSON_AddNumberToObject(jitem,"extent_maximum",nbtiles_extent_max);
///          cJSON_AddNumberToObject(jitem,"extent_cached",nbtiles_extent_cached);
          cJSON_AddItemToObject(jfile, "nb_tiles", jitem);
///          cJSON_AddNumberToObject(jfile, "coverage", coverage);
        }

        // Add tiles in file to tiles in zoom level
        tiles_max_in_level += tiles_max_in_file;
        tiles_cached_in_level += tiles_cached_in_file;

        // Release resources that are no longer needed
        GEOSGeom_destroy(file_geom);
        GEOSGeom_destroy(region_in_file_geom);
      }
    }

    // Add tiles in level to tiles in cache
    tiles_max_in_cache += tiles_max_in_level;
    tiles_cached_in_cache += tiles_cached_in_level;

    // Report coverage information for current zoom level and extent
    if (json_output) {
      coverage = zoom_cached / zoom_max;
      jitem = cJSON_CreateObject();
      cJSON_AddNumberToObject(jitem, "max_in_level", tiles_max_in_level);
      cJSON_AddNumberToObject(jitem, "cached_in_level", tiles_cached_in_level);
      cJSON_AddNumberToObject(jitem, "extent_maximum", zoom_max);
      cJSON_AddNumberToObject(jitem, "extent_cached", zoom_cached);
      cJSON_AddItemToObject(jzoom, "nb_tiles", jitem);
      cJSON_AddNumberToObject(jzoom, "coverage", coverage);
    }
  }


  // Report global coverage information
  if (json_output) {
    apr_off_t tile_size = (apr_off_t)(cache_size/cache_cached);
    apr_off_t missing_size = (apr_off_t)(cache_max-cache_cached)*tile_size;
    coverage = cache_cached / cache_max;
    jitem = cJSON_CreateObject();
    cJSON_AddNumberToObject(jitem, "max_in_cache", tiles_max_in_cache);
    cJSON_AddNumberToObject(jitem, "cached_in_cache", tiles_cached_in_cache);
    cJSON_AddNumberToObject(jitem, "extent_maximum", cache_max);
    cJSON_AddNumberToObject(jitem, "extent_cached", cache_cached);
    cJSON_AddItemToObject(jreport, "nb_tiles", jitem);
    cJSON_AddNumberToObject(jreport, "coverage", coverage);
    cJSON_AddNumberToObject(jreport, "extent_cached_size", cache_size);
    cJSON_AddNumberToObject(jreport, "avg_tile_size", tile_size);
    cJSON_AddNumberToObject(jreport, "estimated_cache_missing_size",
                            missing_size);
  }

  // Last progression indication
  if (show_progress) {
    fprintf(stderr, " Finished: 100.000%% done    \n");
  }

  // Display JSON report
  if (json_output) {
    printf("%s\n", cJSON_Print(jreport));
  }


success:
  _destroy_json_pool();
  finishGEOS();
  apr_terminate();
  return 0;


failure:
  if (GC_HAS_ERROR(&ctx)) {
    fprintf(stderr, "%s: %s\n", base_name(argv[0]),
        ctx.get_error_message(&ctx));
  }
  _destroy_json_pool();
  finishGEOS();
  apr_terminate();
  return 1;
}


// vim: set tw=79 ts=2 sw=2 sts=2 et ai si nonu syn=c fo+=ro :
