#include <stdio.h>
#include <stdlib.h>
#include <apr_general.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <sqlite3.h>
#include "mapcache.h"
#include "ezxml.h"


const apr_getopt_option_t optlist[] = {
  { "verbose", 'v', FALSE, "Display non essential data" },
  { "config",  'c', TRUE,  "Set MapCache configuration file" },
  { "tileset", 't', TRUE,  "Set tileset associated to cache" },
  { "grid",    'g', TRUE,  "Set grid associated to cache" },
  { "dim",     'd', TRUE,  "Set values for dimensions, e.g.:"
                           " \"channel=IR:sensor=spot\"" },
  { "query",   'q', TRUE,  "Set query for counting tiles in a rectangle" },
  { "minx",    'x', TRUE,  "Set lower left X coordinate in grid's SRS" },
  { "miny",    'y', TRUE,  "Set lower left Y coordinate in grid's SRS" },
  { "maxx",    'X', TRUE,  "Set upper right X coordinate in grid's SRS" },
  { "maxy",    'Y', TRUE,  "Set upper right Y coordinate in grid's SRS" },
  { "zoom",    'z', TRUE,  "Set zoom level" },
  { "endofopt", 0,  FALSE, "End of options" }
};


void mapcache_log(mapcache_context *ctx, mapcache_log_level lvl, char *msg, ...)
{
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  fprintf(stderr, "\n");
}


char * str_replace_all(apr_pool_t *pool, const char *string,
                       const char *substr, const char *replacement)
{
  char * replaced = apr_pstrdup(pool, string);
  while (strstr(replaced, substr)) {
    replaced = mapcache_util_str_replace(pool, string, substr, replacement);
  }
  return replaced;
}

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

  // Dimensions, both {dim} and {dim:foo}
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


int count_tiles(mapcache_context * ctx, const char * dbfile,
                const char * count_query, int x, int y, int X, int Y, int z,
                mapcache_grid * grid, mapcache_tileset * tileset,
                apr_array_header_t * dimensions)
{
  sqlite3 * db;
  sqlite3_stmt * res;
  int rc, idx;
  int count;

  rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_busy_timeout(db, 5000);

  rc = sqlite3_prepare_v2(db, count_query, -1, &res, 0);
  if (rc != SQLITE_OK) {
    ctx->set_error(ctx, 500, "SQLite failed: '%s'", sqlite3_errmsg(db));
    sqlite3_close(db);
    return 0;
  }

  idx = sqlite3_bind_parameter_index(res, ":minx");
  if (idx) sqlite3_bind_int(res, idx, x);
  idx = sqlite3_bind_parameter_index(res, ":miny");
  if (idx) sqlite3_bind_int(res, idx, y);
  idx = sqlite3_bind_parameter_index(res, ":maxx");
  if (idx) sqlite3_bind_int(res, idx, X);
  idx = sqlite3_bind_parameter_index(res, ":maxy");
  if (idx) sqlite3_bind_int(res, idx, Y);
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

  return count;
}


int main(int argc, char * argv[])
{
  mapcache_context ctx;
  apr_getopt_t * opt;
  int status;
  int optk;
  const char * optv;
  int verbose = FALSE;
  const char * config_file = NULL;
  const char * tileset_name = NULL;
  const char * grid_name = NULL;
  const char * dim_spec = NULL;
  const char * count_query = NULL;
  mapcache_tileset * tileset;
  mapcache_grid * grid;
  mapcache_cache * cache;
  apr_array_header_t * dimensions = NULL;
  char * cache_dbfile = NULL;
  apr_hash_t * xyz_fmt;
  int i, ix, iy;
  ezxml_t doc, node;
  ezxml_t cache_node = NULL;
  ezxml_t dbfile_node = NULL;
  char * text;
  apr_hash_index_t * hi;
  int cache_xcount, cache_ycount;
  double minx=0, miny=0, maxx=0, maxy=0;
  int z=0;
  int pix_minx, pix_miny, pix_maxx, pix_maxy;
  int til_minx, til_miny, til_maxx, til_maxy;
  int db_minx, db_miny, db_maxx, db_maxy;
  double total_tile_nb, present_tile_nb;
  apr_array_header_t * dbfiles_for_bbox;


  // Initialize Apache runtime and Mapcache context
  apr_initialize();
  apr_pool_create(&ctx.pool, NULL);
  mapcache_context_init(&ctx);
  ctx.config = mapcache_configuration_create(ctx.pool);
  ctx.log = mapcache_log;


  // Parse command-line options
  apr_getopt_init(&opt, ctx.pool, argc, (const char*const*)argv);
  while ((status = apr_getopt_long(opt, optlist, &optk, &optv)) == APR_SUCCESS)
  {
    switch (optk) {
      case 'v': // --verbose
        verbose = TRUE;
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
      case 'd': // --dim <dim_spec>
        dim_spec = optv;
        break;
      case 'q': // --query <count_query>
        count_query = optv;
        break;
      case 'x': // --minx <minx>
        minx = strtod(optv, &text);
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad double format for --minx option: %s",
                        optv);
          goto failure;
        }
        break;
      case 'y': // --miny <miny>
        miny = strtod(optv, &text);
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad double format for --miny option: %s",
                        optv);
          goto failure;
        }
        break;
      case 'X': // --maxx <maxx>
        maxx = strtod(optv, &text);
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad double format for --maxx option: %s",
                        optv);
          goto failure;
        }
        break;
      case 'Y': // --maxy <maxy>
        maxy = strtod(optv, &text);
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad double format for --maxy option: %s",
                        optv);
          goto failure;
        }
        break;
      case 'z': // --zoom <z>
        z = strtol(optv, &text, 10);
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad int format for -z option: %s", optv);
          goto failure;
        }
        break;

    }
  }
  if (status != APR_EOF) {
    ctx.set_error(&ctx, 500, "Bad options");
    goto failure;
  }


  // Load Mapcache configuration file in Mapcache internal data structure
  if (!config_file) {
    ctx.set_error(&ctx, 500, "Configuration file has not been specified"
                             " (need: --config <file>)");
    goto failure;
  }
  mapcache_configuration_parse(&ctx, config_file, ctx.config, 0);
  if (ctx.get_error(&ctx)) goto failure;


  // Load MapCache configuration again, this time as an XML document, in order
  // to gain access to settings that are unreacheable from Mapcache API
  doc = ezxml_parse_file(config_file);


  // Retrieve tileset information
  if (!tileset_name) {
    ctx.set_error(&ctx, 500, "tileset has not been specified"
                             " (need: --tileset <name>)");
    goto failure;
  }
  tileset = mapcache_configuration_get_tileset(ctx.config, tileset_name);
  if (!tileset) {
    ctx.set_error(&ctx, 500, "tileset \"%s\" has not been found"
                             " in configuration", tileset_name);
    goto failure;
  }


  // Retrieve grid information
  if (!grid_name) {
    ctx.set_error(&ctx, 500, "grid has not been specified"
                             " (need: --grid <name>)");
    goto failure;
  }
  for (i=0 ; i<tileset->grid_links->nelts ; i++) {
    mapcache_grid_link * grid_link;
    grid_link = APR_ARRAY_IDX(tileset->grid_links, i, mapcache_grid_link*);
    if (strcmp(grid_link->grid->name, grid_name)==0) {
      grid = grid_link->grid;
      break;
    }
  }
  if (!grid) {
    ctx.set_error(&ctx, 500, "grid \"%s\" has not been found in \"%s\""
                             " tileset config.", grid_name, tileset->name);
    goto failure;
  }


  // Retrieve cache information
  cache = tileset->_cache;
  if (cache->type != MAPCACHE_CACHE_SQLITE) {
    ctx.set_error(&ctx, 500, "cache \"%s\" of tileset \"%s\" is not of"
                             " type SQLite", cache->name, tileset->name);
    goto failure;
  }
  for (node = ezxml_child(doc, "cache") ; node ; node = node->next) {
    if (strcmp(ezxml_attr(node, "name"), cache->name) == 0) {
      cache_node = node;
      break;
    }
  }
  if (!cache_node) {
    ctx.set_error(&ctx, 500, "cache \"%s\" has not been not found",
                             cache->name);
    goto failure;
  }
  dbfile_node = ezxml_child(cache_node, "dbfile");
  cache_dbfile = dbfile_node->txt;
  if (!cache_dbfile) {
    ctx.set_error(&ctx, 500, "Failed to parse <dbfile> tag of cache \"%s\"",
                             cache->name);
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
    const char *key, *val;
    apr_hash_this(hi, (const void**)&key, NULL, (void**)&val);
    val = ezxml_attr(dbfile_node, key);
    if (!val) val = "%d";
    apr_hash_set(xyz_fmt, key, APR_HASH_KEY_STRING, val);
  }
  cache_xcount = cache_ycount = -1;
  text = ezxml_child(node, "xcount")->txt;
  if (text) cache_xcount = (int)strtol(text, NULL, 10);
  text = ezxml_child(node, "ycount")->txt;
  cache_ycount = (int)strtol(text, NULL, 10);


  // Retrieve dimensions information
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
    // Update dimensions with values specified with --dim command line option
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
          ctx.set_error(&ctx, 500, "Can't parse dimension settings: %s\n",
                                   dim_spec);
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
        ctx.set_error(&ctx, 500, "invalid value \"%s\" for dimension \"%s\"\n",
                      entry->requested_value, entry->dimension->name);
        goto failure;
      }
    }
  }


  // Set default query for counting tiles in a SQLite cache file
  if (!count_query) {
    count_query = "SELECT count(rowid)"
                  "  FROM tiles"
                  " WHERE (x between :minx and :maxx)"
                  "   AND (y between :miny and :maxy)"
                  "   AND tileset=:tileset AND grid=:grid AND dim=:dim";
  }


  // Check requested bounding box and zoom level with respect to grid extent
  if (minx < grid->extent.minx || minx > grid->extent.maxx) {
    ctx.set_error(&ctx, 500, "Lower left X coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", minx,
                             grid->extent.minx, grid->extent.maxx);
    goto failure;
  }
  if (miny < grid->extent.miny || miny > grid->extent.maxy) {
    ctx.set_error(&ctx, 500, "Lower left Y coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", miny,
                             grid->extent.miny, grid->extent.maxy);
    goto failure;
  }
  if (maxx < grid->extent.minx || maxx > grid->extent.maxx) {
    ctx.set_error(&ctx, 500, "Upper right X coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", maxx,
                             grid->extent.minx, grid->extent.maxx);
    goto failure;
  }
  if (maxy < grid->extent.miny || maxy > grid->extent.maxy) {
    ctx.set_error(&ctx, 500, "Upper right Y coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", maxy,
                             grid->extent.miny, grid->extent.maxy);
    goto failure;
  }
  if (z < 0 || z >= grid->nlevels) {
    ctx.set_error(&ctx, 500, "Zoom level %d not in valid interval [ %d, %d ]",
                  z, 0, grid->nlevels-1);
    goto failure;
  }
  // Swap bounds if inverted
  if (minx > maxx) {
    double swap = minx;
    minx = maxx;
    maxx = swap;
  }
  if (miny > maxy) {
    double swap = miny;
    miny = maxy;
    maxy = swap;
  }
  // Convert bounding box coordinates in pixels, tiles and DB files
  pix_minx = (minx - grid->extent.minx) / grid->levels[z]->resolution;
  pix_miny = (miny - grid->extent.miny) / grid->levels[z]->resolution;
  pix_maxx = (maxx - grid->extent.minx) / grid->levels[z]->resolution;
  pix_maxy = (maxy - grid->extent.miny) / grid->levels[z]->resolution;
  til_minx = pix_minx / grid->tile_sx;
  til_miny = pix_miny / grid->tile_sy;
  til_maxx = pix_maxx / grid->tile_sx;
  til_maxy = pix_maxy / grid->tile_sy;
  db_minx = til_minx / cache_xcount;
  db_miny = til_miny / cache_ycount;
  db_maxx = til_maxx / cache_xcount;
  db_maxy = til_maxy / cache_ycount;
  // Count total number of tiles in bounding box
  total_tile_nb = (double)(til_maxx-til_minx+1)*(double)(til_maxy-til_miny+1);


  // List DB files containing portions of bounding box
  // and count cached tiles belonging to bounding box
  present_tile_nb = 0;
  dbfiles_for_bbox = apr_array_make(ctx.pool, 1, sizeof(char*));
  for (ix = db_minx ; ix <= db_maxx ; ix++) {
    for (iy = db_miny ; iy <= db_maxy ; iy++) {
      int x;
      int y;
      char * dbfile;
      int cov;
      x = ix * cache_xcount;
      y = iy * cache_ycount;
      dbfile = dbfilename(ctx.pool, cache_dbfile, tileset, grid, dimensions,
                          xyz_fmt, z, x, y, cache_xcount, cache_ycount);
      APR_ARRAY_PUSH(dbfiles_for_bbox, char*) = dbfile;
      cov = count_tiles(&ctx, dbfile, count_query, til_minx, til_miny,
                        til_maxx, til_maxy, z, grid, tileset, dimensions);
      present_tile_nb += cov;
    }
  }


  // Display problem input
  if (verbose) {
    printf("Zoom_level: %d\n", z);
    printf("Bounding_box:\n");
    printf("  grid_coordinates : [ %.18g, %.18g, %.18g, %.18g ]\n",
                                    minx, miny, maxx, maxy);
    printf("  pixel_coordinates: [ %d, %d, %d, %d ]\n",
                                    pix_minx, pix_miny, pix_maxx, pix_maxy);
    printf("  tile_coordinates : [ %d, %d, %d, %d ]\n",
                                    til_minx, til_miny, til_maxx, til_maxy);
    printf("  DB_coordinates   : [ %d, %d, %d, %d ]\n",
                                    db_minx, db_miny, db_maxx, db_maxy);
  }


  // Display coverage
  if (verbose) {
    printf("  Coverage:\n");
    printf("    total_number_of_tiles   : %.18g\n", total_tile_nb);
    printf("    number_of_tiles_in_cache: %.18g\n", present_tile_nb);
    printf("    ratio                   : %3.5g%%\n",
                                          present_tile_nb/total_tile_nb*100);
    printf("    DB_files:\n");
    for ( i=0 ; i < dbfiles_for_bbox->nelts ; i++ ) {
       printf("    - %s\n", APR_ARRAY_IDX(dbfiles_for_bbox, i, char*));
    }
  } else {
    printf("%3.5g%% %.18g %.18g\n", present_tile_nb/total_tile_nb*100,
                                    present_tile_nb, total_tile_nb);
  }


// success:
  apr_terminate();
  return 0;


failure:
  fprintf(stderr, "%s: %s\n", argv[0], ctx.get_error_message(&ctx));
  apr_terminate();
  return 1;
}
