/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: tiled tiff filesytem cache backend.
 * Author:   Thomas Bonfort
 *           Frank Warmerdam
 *           Even Rouault
 *           MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 2011-2017 Regents of the University of Minnesota.
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
#ifdef USE_TIFF

#include <apr_file_info.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <tiffio.h>

#ifdef USE_GDAL
#include "cpl_vsi.h"
#include "cpl_conv.h"
#define CPL_SERV_H_INCLUDED
#endif

#ifdef USE_GEOTIFF
#include "xtiffio.h"
#include "geovalues.h"
#include "geotiff.h"
#include "geo_normalize.h"
#include "geo_tiffp.h"
#include "geo_keyp.h"
#define MyTIFFOpen XTIFFOpen
#define MyTIFFClose XTIFFClose
#else
#define MyTIFFOpen TIFFOpen
#define MyTIFFClose TIFFClose
#endif

typedef enum
{
    MAPCACHE_TIFF_STORAGE_FILE,
    MAPCACHE_TIFF_STORAGE_REST,
    MAPCACHE_TIFF_STORAGE_GOOGLE
} mapcache_cache_tiff_storage_type;


typedef struct mapcache_cache_tiff mapcache_cache_tiff;

struct mapcache_cache_tiff {
  mapcache_cache cache;
  char *filename_template;
  char *x_fmt,*y_fmt,*z_fmt,*inv_x_fmt,*inv_y_fmt,*div_x_fmt,*div_y_fmt,*inv_div_x_fmt,*inv_div_y_fmt;
  int count_x;
  int count_y;
  mapcache_image_format_jpeg *format;
  mapcache_locker *locker;
  struct {
    mapcache_cache_tiff_storage_type type;
    int connection_timeout;
    int timeout;
    char *header_file;
    union
    {
        struct
        {
            char* access;
            char* secret;
        } google;
    } u;
  } storage;
};

#ifdef USE_GDAL

static tsize_t
_tiffReadProc( thandle_t th, tdata_t buf, tsize_t size )
{
    VSILFILE* fp = (VSILFILE*)th;
    return VSIFReadL( buf, 1, size, fp );
}

static tsize_t
_tiffWriteProc( thandle_t th, tdata_t buf, tsize_t size )
{
    VSILFILE* fp = (VSILFILE*)th;
    return VSIFWriteL( buf, 1, size, fp );
}

static toff_t
_tiffSeekProc( thandle_t th, toff_t off, int whence )
{
    VSILFILE* fp = (VSILFILE*)th;
    if( VSIFSeekL( fp, off, whence ) != 0 )
    {
        TIFFErrorExt( th, "_tiffSeekProc", "%s", VSIStrerror( errno ) );
        return (toff_t)( -1 );
    }
    return VSIFTellL( fp );
}

static int
_tiffCloseProc( thandle_t th )
{
    VSILFILE* fp = (VSILFILE*)th;
    VSIFCloseL(fp);
    return 0;
}

static toff_t
_tiffSizeProc( thandle_t th )
{
    vsi_l_offset old_off;
    toff_t file_size;
    VSILFILE* fp = (VSILFILE*)th;

    old_off = VSIFTellL( fp );
    (void)VSIFSeekL( fp, 0, SEEK_END );

    file_size = (toff_t) VSIFTellL( fp );
    (void)VSIFSeekL( fp, old_off, SEEK_SET );

    return file_size;
}

static int
_tiffMapProc( thandle_t th, tdata_t* pbase, toff_t* psize )
{
    (void)th;
    (void)pbase;
    (void)psize;
    /* Unimplemented */
    return 0;
}

static void
_tiffUnmapProc( thandle_t th, tdata_t base, toff_t size )
{
    (void)th;
    (void)base;
    (void)size;
    /* Unimplemented */
}

static char* set_conf_value(const char* key, const char* value)
{
    const char* old_val_const;
    char* old_val = NULL;
    old_val_const = CPLGetConfigOption(key, NULL);
    if( old_val_const != NULL )
        old_val = strdup(old_val_const);
    /* Prevent a directory listing to be done */
    CPLSetConfigOption(key, value);
    return old_val;
}

static void restore_conf_value(const char* key, char* old_val)
{
    CPLSetConfigOption(key, old_val);
    free(old_val);
}

typedef struct
{
    char* old_val_disable_readdir;
    char* old_val_headerfile;
    char* old_val_secret;
    char* old_val_access;
} mapache_gdal_env_context;

static void set_gdal_context(mapcache_cache_tiff *cache,
                             mapache_gdal_env_context* pcontext)
{
    memset(pcontext, 0, sizeof(mapache_gdal_env_context));
    /* Prevent a directory listing to be done */
    pcontext->old_val_disable_readdir =
        set_conf_value("GDAL_DISABLE_READDIR_ON_OPEN", "YES");

    if( cache->storage.header_file ) {
        pcontext->old_val_headerfile = set_conf_value("GDAL_HTTP_HEADER_FILE",
                                            cache->storage.header_file);
    }
    if( cache->storage.type == MAPCACHE_TIFF_STORAGE_GOOGLE ) {
        pcontext->old_val_secret = set_conf_value("GS_SECRET_ACCESS_KEY",
                                        cache->storage.u.google.secret);
        pcontext->old_val_access = set_conf_value("GS_ACCESS_KEY_ID",
                                        cache->storage.u.google.access);
    }
}

static void restore_gdal_context(mapcache_cache_tiff *cache,
                                 mapache_gdal_env_context* pcontext)
{

    restore_conf_value("GDAL_DISABLE_READDIR_ON_OPEN",
                       pcontext->old_val_disable_readdir);
    if( cache->storage.header_file ) {
        restore_conf_value("GDAL_HTTP_HEADER_FILE",
                           pcontext->old_val_headerfile);
    }
    if( cache->storage.type == MAPCACHE_TIFF_STORAGE_GOOGLE ) {
        restore_conf_value("GS_SECRET_ACCESS_KEY",
                           pcontext->old_val_secret);
        restore_conf_value("GS_ACCESS_KEY_ID",
                           pcontext->old_val_access);
    }
}

static int mapcache_cache_tiff_vsi_stat(
                                      mapcache_cache_tiff *cache,
                                      const char* name,
                                      VSIStatBufL* pstat)
{
    mapache_gdal_env_context context;
    int ret;

    set_gdal_context(cache, &context);
    ret = VSIStatL(name, pstat);
    restore_gdal_context(cache, &context);

    return ret;
}

static VSILFILE* mapcache_cache_tiff_vsi_open(
                                      mapcache_cache_tiff *cache,
                                      const char* name, const char* mode )
{
    mapache_gdal_env_context context;
    VSILFILE* fp;

    set_gdal_context(cache, &context);
    fp = VSIFOpenL(name, mode);
    restore_gdal_context(cache, &context);

    return fp;
}

static TIFF* mapcache_cache_tiff_open(mapcache_context *ctx,
                                      mapcache_cache_tiff *cache,
                                      const char* name, const char* mode )
{
    char chDummy;
    VSILFILE* fp;

    /* If writing or using a regular filename, then use standard */
    /* libtiff/libgeotiff I/O layer */
    if( strcmp(mode, "r") != 0 || strncmp(name, "/vsi", 4) != 0 )
    {
        return MyTIFFOpen(name, mode);
    }

    fp = mapcache_cache_tiff_vsi_open(cache, name, mode);
    if( fp == NULL )
        return NULL;

    if( strcmp(mode, "r") == 0 )
    {
        /* But then the file descriptor may point to an invalid resource */
        /* so try reading a byte from it */
        if(VSIFReadL(&chDummy, 1, 1, fp) != 1)
        {
            VSIFCloseL(fp);
            return NULL;
        }
        VSIFSeekL(fp, 0, SEEK_SET);
    }

    return
#ifdef USE_GEOTIFF
        XTIFFClientOpen
#else
        TIFFClientOpen
#endif
                        ( name, mode,
                         (thandle_t) fp,
                         _tiffReadProc, _tiffWriteProc,
                         _tiffSeekProc, _tiffCloseProc, _tiffSizeProc,
                         _tiffMapProc, _tiffUnmapProc );
}

static void CPL_STDCALL mapcache_cache_tiff_gdal_error_handler(CPLErr eErr,
                                                   int error_num,
                                                   const char* pszMsg)
{
#ifdef DEBUG
    mapcache_context *ctx = (mapcache_context *) CPLGetErrorHandlerUserData();
    ctx->log(ctx,MAPCACHE_DEBUG,"GDAL %s, %d: %s",
             (eErr == CE_Failure) ? "Failure":
             (eErr == CE_Warning) ? "Warning":
                                    "Debug",
             error_num,
             pszMsg);
#endif
}


#else

static TIFF* mapcache_cache_tiff_open(mapcache_context *ctx,
                                      mapcache_cache_tiff *cache,
                                      const char* name, const char* mode )
{
    (void)ctx;
    (void)cache;
    return MyTIFFOpen(name, mode);

}

#endif /* USE_GDAL */

#undef MyTIFFOpen

/**
 * \brief return filename for given tile
 *
 * \param tile the tile to get the key from
 * \param path pointer to a char* that will contain the filename
 * \param r
 * \private \memberof mapcache_cache_tiff
 */
static void _mapcache_cache_tiff_tile_key(mapcache_context *ctx, mapcache_cache_tiff *cache, mapcache_tile *tile, char **path)
{
  if( cache->storage.type == MAPCACHE_TIFF_STORAGE_REST ) {
    *path = apr_pstrcat(ctx->pool, "/vsicurl/", cache->filename_template, NULL);
  }
  else if( cache->storage.type == MAPCACHE_TIFF_STORAGE_GOOGLE &&
           strncmp(cache->filename_template,
                   "https://storage.googleapis.com/",
                   strlen("https://storage.googleapis.com/")) == 0) {
    *path = apr_pstrcat(ctx->pool, "/vsigs/",
        cache->filename_template +
            strlen("https://storage.googleapis.com/"), NULL);
  }
  else {
    *path = apr_pstrdup(ctx->pool, cache->filename_template);
  }

  /*
   * generic template substitutions
   */
  if(strstr(*path,"{tileset}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}",
                                      tile->tileset->name);
  if(strstr(*path,"{grid}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}",
                                      tile->grid_link->grid->name);
  if(tile->dimensions && strstr(*path,"{dim")) {
    char *dimstring="";
    int i = tile->dimensions->nelts;
    while(i--) {
      mapcache_requested_dimension *rdim = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
      const char *dimval = mapcache_util_str_sanitize(ctx->pool,rdim->cached_value,"/.",'#');
      char *dim_key = apr_pstrcat(ctx->pool,"{dim:",rdim->dimension->name,"}",NULL);
      dimstring = apr_pstrcat(ctx->pool,dimstring,"#",dimval,NULL);
      if(strstr(*path,dim_key)) {
        *path = mapcache_util_str_replace(ctx->pool,*path, dim_key, dimval);
      }
    }
    *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
  }


  while(strstr(*path,"{z}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
                                      apr_psprintf(ctx->pool,cache->z_fmt,tile->z));
  /*
   * x and y replacing, when the tiff files are numbered with an increasing
   * x,y scheme (adjacent tiffs have x-x'=1 or y-y'=1
   */
  while(strstr(*path,"{div_x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{div_x}",
                                      apr_psprintf(ctx->pool,cache->div_x_fmt,tile->x/cache->count_x));
  while(strstr(*path,"{div_y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{div_y}",
                                      apr_psprintf(ctx->pool,cache->div_y_fmt,tile->y/cache->count_y));
  while(strstr(*path,"{inv_div_y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_div_y}",
                                      apr_psprintf(ctx->pool,cache->inv_div_y_fmt,(tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1)/cache->count_y));
  while(strstr(*path,"{inv_div_x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_div_x}",
                                      apr_psprintf(ctx->pool,cache->inv_div_x_fmt,(tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1)/cache->count_x));

  /*
   * x and y replacing, when the tiff files are numbered with the index
   * of their bottom-left tile
   * adjacent tiffs have x-x'=count_x or y-y'=count_y
   */
  while(strstr(*path,"{x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                                      apr_psprintf(ctx->pool,cache->x_fmt,tile->x/cache->count_x*cache->count_x));
  while(strstr(*path,"{y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                                      apr_psprintf(ctx->pool,cache->y_fmt,tile->y/cache->count_y*cache->count_y));
  while(strstr(*path,"{inv_y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                                      apr_psprintf(ctx->pool,cache->inv_y_fmt,(tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1)/cache->count_y*cache->count_y));
  while(strstr(*path,"{inv_x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                                      apr_psprintf(ctx->pool,cache->inv_x_fmt,(tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1)/cache->count_x*cache->count_y));
  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

#ifdef DEBUG
static void check_tiff_format(mapcache_context *ctx, mapcache_cache_tiff *cache, mapcache_tile *tile, TIFF *hTIFF, const char *filename)
{
  uint32 imwidth,imheight,tilewidth,tileheight;
  int16 planarconfig,orientation;
  uint16 compression;
  uint16 photometric;
  int rv;
  mapcache_grid_level *level;
  int ntilesx;
  int ntilesy;
  TIFFGetField( hTIFF, TIFFTAG_IMAGEWIDTH, &imwidth );
  TIFFGetField( hTIFF, TIFFTAG_IMAGELENGTH, &imheight );
  TIFFGetField( hTIFF, TIFFTAG_TILEWIDTH, &tilewidth );
  TIFFGetField( hTIFF, TIFFTAG_TILELENGTH, &tileheight );

  /* Test that the TIFF is tiled and not stripped */
  if(!TIFFIsTiled(hTIFF)) {
    ctx->set_error(ctx,500,"TIFF file \"%s\" is not tiled", filename);
    return;
  }

  /* check we have jpeg compression */
  rv = TIFFGetField( hTIFF, TIFFTAG_COMPRESSION, &compression );
  if(rv == 1 && compression != COMPRESSION_JPEG) {
    ctx->set_error(ctx,500,"TIFF file \"%s\" is not jpeg compressed",
                   filename);
    return;
  }

  /* tiff must be pixel interleaved, not with a single image per band */
  rv = TIFFGetField( hTIFF, TIFFTAG_PLANARCONFIG, &planarconfig );
  if(rv == 1 && planarconfig != PLANARCONFIG_CONTIG) {
    ctx->set_error(ctx,500,"TIFF file \"%s\" is not pixel interleaved",
                   filename);
    return;
  }

  /* is this test needed once we now we have JPEG ? */
  rv = TIFFGetField( hTIFF, TIFFTAG_PHOTOMETRIC, &photometric );
  if(rv == 1 && (photometric != PHOTOMETRIC_RGB && photometric != PHOTOMETRIC_YCBCR)) {
    ctx->set_error(ctx,500,"TIFF file \"%s\" is not RGB: %d",
                   filename);
    return;
  }

  /* the default is top-left, but check just in case */
  rv = TIFFGetField( hTIFF, TIFFTAG_ORIENTATION, &orientation );
  if(rv == 1 && orientation != ORIENTATION_TOPLEFT) {
    ctx->set_error(ctx,500,"TIFF file \"%s\" is not top-left oriented",
                   filename);
    return;
  }

  /* check that the tiff internal tiling aligns with the mapcache_grid we are using:
   * - the tiff tile size must match the grid tile size
   * - the number of tiles in each direction in the tiff must match what has been
   *   configured for the cache
   */
  level = tile->grid_link->grid->levels[tile->z];
  ntilesx = MAPCACHE_MIN(cache->count_x, level->maxx);
  ntilesy = MAPCACHE_MIN(cache->count_y, level->maxy);
  if( tilewidth != tile->grid_link->grid->tile_sx ||
      tileheight != tile->grid_link->grid->tile_sy ||
      imwidth != tile->grid_link->grid->tile_sx * ntilesx ||
      imheight != tile->grid_link->grid->tile_sy * ntilesy ) {
    ctx->set_error(ctx,500,"TIFF file %s imagesize (%d,%d) and tilesize (%d,%d).\
            Expected (%d,%d),(%d,%d)",filename,imwidth,imheight,tilewidth,tileheight,
                   tile->grid_link->grid->tile_sx * ntilesx,
                   tile->grid_link->grid->tile_sy * ntilesy,
                   tile->grid_link->grid->tile_sx,
                   tile->grid_link->grid->tile_sy);
    return;
  }

  /* TODO: more tests ? */

  /* ok, success */
}
#endif

static int _mapcache_cache_tiff_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *filename;
  TIFF *hTIFF;
  mapcache_cache_tiff *cache = (mapcache_cache_tiff*)pcache;
  _mapcache_cache_tiff_tile_key(ctx, cache, tile, &filename);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FALSE;
  }

#ifdef USE_GDAL
  CPLPushErrorHandlerEx(mapcache_cache_tiff_gdal_error_handler, ctx);
#endif

  hTIFF = mapcache_cache_tiff_open(ctx,cache,filename,"r");

  if(hTIFF) {
    do {
      uint32 nSubType = 0;
      int tiff_offx, tiff_offy; /* the x and y offset of the tile inside the tiff image */
      int tiff_off; /* the index of the tile inside the list of tiles of the tiff image */

      mapcache_grid_level *level;
      int ntilesx;
      int ntilesy;
      toff_t  *offsets=NULL, *sizes=NULL;

      if( !TIFFGetField(hTIFF, TIFFTAG_SUBFILETYPE, &nSubType) )
        nSubType = 0;

      /* skip overviews and masks */
      if( (nSubType & FILETYPE_REDUCEDIMAGE) ||
          (nSubType & FILETYPE_MASK) )
        continue;


#ifdef DEBUG
      check_tiff_format(ctx,cache,tile,hTIFF,filename);
      if(GC_HAS_ERROR(ctx)) {
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FALSE;
      }
#endif
      level = tile->grid_link->grid->levels[tile->z];
      ntilesx = MAPCACHE_MIN(cache->count_x, level->maxx);
      ntilesy = MAPCACHE_MIN(cache->count_y, level->maxy);

      /* x offset of the tile along a row */
      tiff_offx = tile->x % ntilesx;

      /*
       * y offset of the requested row. we inverse it as the rows are ordered
       * from top to bottom, whereas the tile y is bottom to top
       */
      tiff_offy = ntilesy - (tile->y % ntilesy) -1;
      tiff_off = tiff_offy * ntilesx + tiff_offx;

      if(1 != TIFFGetField( hTIFF, TIFFTAG_TILEOFFSETS, &offsets )) {
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FALSE;
      }
      if(1 != TIFFGetField( hTIFF, TIFFTAG_TILEBYTECOUNTS, &sizes )) {
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FALSE;
      }
      MyTIFFClose(hTIFF);
      if( offsets[tiff_off] > 0 && sizes[tiff_off] > 0 ) {
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_TRUE;
      } else {
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FALSE;
      }
    } while( TIFFReadDirectory( hTIFF ) );
     /* TIFF only contains overviews ? */
  }
#ifdef USE_GDAL
  CPLPopErrorHandler();
#endif
  return MAPCACHE_FALSE;
}

static void _mapcache_cache_tiff_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  ctx->set_error(ctx,500,"TIFF cache tile deleting not implemented");
}


/**
 * \brief get file content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored in the file
 * \private \memberof mapcache_cache_tiff
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_tiff_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *filename;
  TIFF *hTIFF = NULL;
  int rv;
  mapcache_cache_tiff *cache = (mapcache_cache_tiff*)pcache;
  _mapcache_cache_tiff_tile_key(ctx, cache, tile, &filename);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FALSE;
  }
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"tile (%d,%d,%d) => filename %s)",
           tile->x,tile->y,tile->z,filename);
#endif

#ifdef USE_GDAL
  CPLPushErrorHandlerEx(mapcache_cache_tiff_gdal_error_handler, ctx);
#endif

  hTIFF = mapcache_cache_tiff_open(ctx,cache,filename,"r");

  /*
   * we currrently have no way of knowing if the opening failed because the tif
   * file does not exist (which is not an error condition, as it only signals
   * that the requested tile does not exist in the cache), or if an other error
   * that should be signaled occurred (access denied, not a tiff file, etc...)
   *
   * we ignore this case here and hope that further parts of the code will be
   * able to detect what's happening more precisely
   */

  if(hTIFF) {
    do {
      uint32 nSubType = 0;
      int tiff_offx, tiff_offy; /* the x and y offset of the tile inside the tiff image */
      int tiff_off; /* the index of the tile inside the list of tiles of the tiff image */

      mapcache_grid_level *level;
      int ntilesx;
      int ntilesy;
      toff_t  *offsets=NULL, *sizes=NULL;

      if( !TIFFGetField(hTIFF, TIFFTAG_SUBFILETYPE, &nSubType) )
        nSubType = 0;

      /* skip overviews */
      if( nSubType & FILETYPE_REDUCEDIMAGE )
        continue;


#ifdef DEBUG
      check_tiff_format(ctx,cache,tile,hTIFF,filename);
      if(GC_HAS_ERROR(ctx)) {
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FAILURE;
      }
#endif
      /*
       * compute the width and height of the full tiff file. This
       * is not simply the tile size times the number of tiles per
       * file for lower zoom levels
       */
      level = tile->grid_link->grid->levels[tile->z];
      ntilesx = MAPCACHE_MIN(cache->count_x, level->maxx);
      ntilesy = MAPCACHE_MIN(cache->count_y, level->maxy);

      /* x offset of the tile along a row */
      tiff_offx = tile->x % ntilesx;

      /*
       * y offset of the requested row. we inverse it as the rows are ordered
       * from top to bottom, whereas the tile y is bottom to top
       */
      tiff_offy = ntilesy - (tile->y % ntilesy) -1;
      tiff_off = tiff_offy * ntilesx + tiff_offx;

      /* get the offset of the jpeg data from the start of the file for each tile */
      rv = TIFFGetField( hTIFF, TIFFTAG_TILEOFFSETS, &offsets );
      if( rv != 1 ) {
        ctx->set_error(ctx,500,"Failed to read TIFF file \"%s\" tile offsets",
                       filename);
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FAILURE;
      }

      /* get the size of the jpeg data for each tile */
      rv = TIFFGetField( hTIFF, TIFFTAG_TILEBYTECOUNTS, &sizes );
      if( rv != 1 ) {
        ctx->set_error(ctx,500,"Failed to read TIFF file \"%s\" tile sizes",
                       filename);
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_FAILURE;
      }

      /*
       * the tile data exists for the given tiff_off if both offsets and size
       * are not zero for that index.
       * if not, the tiff file is sparse and is missing the requested tile
       */
      if( offsets[tiff_off] > 0 && sizes[tiff_off] >= 2 ) {
        apr_file_t *f;
        apr_finfo_t finfo;
        apr_status_t ret;

        /* read the jpeg header (common to all tiles) */
        uint32 jpegtable_size = 0;
        unsigned char* jpegtable_ptr;
        rv = TIFFGetField( hTIFF, TIFFTAG_JPEGTABLES, &jpegtable_size, &jpegtable_ptr );
        if( rv != 1 || !jpegtable_ptr || jpegtable_size < 2) {
          /* there is no common jpeg header in the tiff tags */
          ctx->set_error(ctx,500,"Failed to read TIFF file \"%s\" jpeg table",
                         filename);
          MyTIFFClose(hTIFF);
#ifdef USE_GDAL
          CPLPopErrorHandler();
#endif
          return MAPCACHE_FAILURE;
        }

        /*
         * open the tiff file directly to access the jpeg image data with the given
         * offset
         */
#ifdef USE_GDAL
        if( cache->storage.type != MAPCACHE_TIFF_STORAGE_FILE )
        {
          char *bufptr;
          apr_off_t off;
          apr_size_t bytes_to_read;
          size_t bytes_read;
          VSIStatBufL sStat;
          VSILFILE* fp;

          fp = mapcache_cache_tiff_vsi_open(cache, filename, "r");
          if( fp == NULL )
          {
            /*
             * shouldn't usually happen. we managed to open the file before,
             * nothing much to do except bail out.
             */
            ctx->set_error(ctx,500,
                           "VSIFOpenL() failed on already open tiff "
                           "file \"%s\", giving up .... ",
                           filename);
            MyTIFFClose(hTIFF);
            CPLPopErrorHandler();
            return MAPCACHE_FAILURE;
          }

          if( mapcache_cache_tiff_vsi_stat(cache, filename, &sStat) == 0 )  {
            /*
             * extract the file modification time. this isn't guaranteed to be the
             * modification time of the actual tile, but it's the best we can do
             */
            tile->mtime = sStat.st_mtime;
          }

#ifdef DEBUG
          ctx->log(ctx,MAPCACHE_DEBUG,"tile (%d,%d,%d) => mtime = %d)",
                   tile->x,tile->y,tile->z,tile->mtime);
#endif


          /* create a memory buffer to contain the jpeg data */
          tile->encoded_data = mapcache_buffer_create(
              (jpegtable_size+sizes[tiff_off]-4),ctx->pool);

          /*
           * copy the jpeg header to the beginning of the memory buffer,
           * omitting the last 2 bytes
           */
          memcpy(tile->encoded_data->buf,jpegtable_ptr,(jpegtable_size-2));

          /* advance the data pointer to after the header data */
          bufptr = ((char *)tile->encoded_data->buf) + (jpegtable_size-2);


          /* go to the specified offset in the tiff file, plus 2 bytes */
          off = offsets[tiff_off]+2;
          VSIFSeekL(fp, (vsi_l_offset)off, SEEK_SET);

          /*
           * copy the jpeg body at the end of the memory buffer, accounting
           * for the two bytes we omitted in the previous step
           */
          bytes_to_read = sizes[tiff_off]-2;
          bytes_read = VSIFReadL(bufptr, 1, bytes_to_read, fp);

          /* check we have correctly read the requested number of bytes */
          if(bytes_to_read != bytes_read) {
            ctx->set_error(ctx,500,"failed to read jpeg body in \"%s\".\
                        (read %d of %d bytes)",
                        filename,(int)bytes_read,(int)sizes[tiff_off]-2);
            VSIFCloseL(fp);
            MyTIFFClose(hTIFF);
            CPLPopErrorHandler();
            return MAPCACHE_FAILURE;
          }

          tile->encoded_data->size = (jpegtable_size+sizes[tiff_off]-4);

          VSIFCloseL(fp);
          CPLPopErrorHandler();
          return MAPCACHE_SUCCESS;
        }
        else
#endif
        if((ret=apr_file_open(&f, filename,
                              APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,APR_OS_DEFAULT,
                              ctx->pool)) == APR_SUCCESS) {
          char *bufptr;
          apr_off_t off;
          apr_size_t bytes;
          ret = apr_file_info_get(&finfo, APR_FINFO_MTIME, f);
          if(ret == APR_SUCCESS) {
            /*
             * extract the file modification time. this isn't guaranteed to be the
             * modification time of the actual tile, but it's the best we can do
             */
            tile->mtime = finfo.mtime;
          }

          /* create a memory buffer to contain the jpeg data */
          tile->encoded_data = mapcache_buffer_create((jpegtable_size+sizes[tiff_off]-4),ctx->pool);

          /*
           * copy the jpeg header to the beginning of the memory buffer,
           * omitting the last 2 bytes
           */
          memcpy(tile->encoded_data->buf,jpegtable_ptr,(jpegtable_size-2));

          /* advance the data pointer to after the header data */
          bufptr = ((char *)tile->encoded_data->buf) + (jpegtable_size-2);


          /* go to the specified offset in the tiff file, plus 2 bytes */
          off = offsets[tiff_off]+2;
          apr_file_seek(f,APR_SET,&off);

          /*
           * copy the jpeg body at the end of the memory buffer, accounting
           * for the two bytes we omitted in the previous step
           */
          bytes = sizes[tiff_off]-2;
          apr_file_read(f,bufptr,&bytes);

          /* check we have correctly read the requested number of bytes */
          if(bytes !=  sizes[tiff_off]-2) {
            ctx->set_error(ctx,500,"failed to read jpeg body in \"%s\".\
                        (read %d of %d bytes)", filename,bytes,sizes[tiff_off]-2);
            apr_file_close(f);
            MyTIFFClose(hTIFF);
#ifdef USE_GDAL
            CPLPopErrorHandler();
#endif
            return MAPCACHE_FAILURE;
          }

          tile->encoded_data->size = (jpegtable_size+sizes[tiff_off]-4);

          /* finalize and cleanup */
          apr_file_close(f);
          MyTIFFClose(hTIFF);
#ifdef USE_GDAL
          CPLPopErrorHandler();
#endif
          return MAPCACHE_SUCCESS;
        } else {
          /* shouldn't usually happen. we managed to open the file with TIFFOpen,
           * but apr_file_open failed to do so.
           * nothing much to do except bail out.
           */
          ctx->set_error(ctx,500,"apr_file_open failed on already open tiff file \"%s\", giving up .... ",
                         filename);
          MyTIFFClose(hTIFF);
#ifdef USE_GDAL
          CPLPopErrorHandler();
#endif
          return MAPCACHE_FAILURE;
        }
      } else {
        /* sparse tiff file without the requested tile */
        MyTIFFClose(hTIFF);
#ifdef USE_GDAL
        CPLPopErrorHandler();
#endif
        return MAPCACHE_CACHE_MISS;
      }
    } /* loop through the tiff directories if there are multiple ones */
    while( TIFFReadDirectory( hTIFF ) );

    /*
     * should not happen?
     * finished looping through directories and didn't find anything suitable.
     * does the file only contain overviews?
     */
    MyTIFFClose(hTIFF);
  }
#ifdef USE_GDAL
  CPLPopErrorHandler();
#endif
  /* failed to open tiff file */
  return MAPCACHE_CACHE_MISS;
}

/**
 * \brief write tile data to tiff
 *
 * writes the content of mapcache_tile::data to tiff.
 * \returns MAPCACHE_FAILURE if there is no data to write, or if the tile isn't locked
 * \returns MAPCACHE_SUCCESS if the tile has been successfully written to tiff
 * \private \memberof mapcache_cache_tiff
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_tiff_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
#ifdef USE_TIFF_WRITE
  char *filename;
  TIFF *hTIFF = NULL;
  int rv;
  void *lock;
  int create;
  mapcache_cache_tiff *cache;
  mapcache_image_format_jpeg *format;
  int tilew;
  int tileh;
  unsigned char *rgb;
  int r,c;
  apr_finfo_t finfo;
  mapcache_grid_level *level;
  int ntilesx;
  int ntilesy;
  int tiff_offx, tiff_offy; /* the x and y offset of the tile inside the tiff image */
  int tiff_off; /* the index of the tile inside the list of tiles of the tiff image */

  cache = (mapcache_cache_tiff*)pcache;
  if( cache->storage.type != MAPCACHE_TIFF_STORAGE_FILE )
  {
    ctx->set_error(ctx,500,"tiff cache %s is read-only\n",pcache->name);
    return;
  }
  _mapcache_cache_tiff_tile_key(ctx, cache, tile, &filename);
  format = (mapcache_image_format_jpeg*) cache->format;
  if(GC_HAS_ERROR(ctx)) {
    return;
  }
#ifdef DEBUG
  ctx->log(ctx,MAPCACHE_DEBUG,"tile write (%d,%d,%d) => filename %s)",
           tile->x,tile->y,tile->z,filename);
#endif

  /*
   * create the directory where the tiff file will be stored
   */
  mapcache_make_parent_dirs(ctx,filename);
  GC_CHECK_ERROR(ctx);

  tilew = tile->grid_link->grid->tile_sx;
  tileh = tile->grid_link->grid->tile_sy;

  if(!tile->raw_image) {
    tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
    GC_CHECK_ERROR(ctx);
  }

  /* remap xrgb to rgb */
  rgb = (unsigned char*)malloc(tilew*tileh*3);
  for(r=0; r<tile->raw_image->h; r++) {
    unsigned char *imptr = tile->raw_image->data + r * tile->raw_image->stride;
    unsigned char *rgbptr = rgb + r * tilew * 3;
    for(c=0; c<tile->raw_image->w; c++) {
      rgbptr[0] = imptr[2];
      rgbptr[1] = imptr[1];
      rgbptr[2] = imptr[0];
      rgbptr += 3;
      imptr += 4;
    }
  }

  /*
   * aquire a lock on the tiff file.
   */

  while(mapcache_lock_or_wait_for_resource(ctx,(cache->locker?cache->locker:ctx->config->locker),filename, &lock) == MAPCACHE_FALSE);

  /* check if the tiff file exists already */
  rv = apr_stat(&finfo,filename,0,ctx->pool);
  if(!APR_STATUS_IS_ENOENT(rv)) {
    hTIFF = mapcache_cache_tiff_open(ctx,cache,filename,"r+");
    create = 0;
  } else {
    hTIFF = mapcache_cache_tiff_open(ctx,cache,filename,"w+");
    create = 1;
  }
  if(!hTIFF) {
    ctx->set_error(ctx,500,"failed to open/create tiff file %s\n",filename);
    goto close_tiff;
  }


  /*
   * compute the width and height of the full tiff file. This
   * is not simply the tile size times the number of tiles per
   * file for lower zoom levels
   */
  level = tile->grid_link->grid->levels[tile->z];
  ntilesx = MAPCACHE_MIN(cache->count_x, level->maxx);
  ntilesy = MAPCACHE_MIN(cache->count_y, level->maxy);
  if(create) {
#ifdef USE_GEOTIFF
    double  adfPixelScale[3], adfTiePoints[6];
    mapcache_extent bbox;
    GTIF *gtif;
    int x,y;
#endif
    /* populate the TIFF tags if we are creating the file */

    TIFFSetField( hTIFF, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT );
    TIFFSetField( hTIFF, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
    TIFFSetField( hTIFF, TIFFTAG_BITSPERSAMPLE, 8 );
    TIFFSetField( hTIFF, TIFFTAG_COMPRESSION, COMPRESSION_JPEG );
    TIFFSetField( hTIFF, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB );
    TIFFSetField( hTIFF, TIFFTAG_TILEWIDTH, tilew );
    TIFFSetField( hTIFF, TIFFTAG_TILELENGTH, tileh );
    TIFFSetField( hTIFF, TIFFTAG_IMAGEWIDTH, ntilesx * tilew );
    TIFFSetField( hTIFF, TIFFTAG_IMAGELENGTH, ntilesy * tileh );
    TIFFSetField( hTIFF, TIFFTAG_SAMPLESPERPIXEL,3 );

#ifdef USE_GEOTIFF
    gtif = GTIFNew(hTIFF);
    if(gtif) {

      GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1,
                 RasterPixelIsArea);

      GTIFKeySet( gtif, GeographicTypeGeoKey, TYPE_SHORT, 1,
                  0 );
      GTIFKeySet( gtif, GeogGeodeticDatumGeoKey, TYPE_SHORT,
                  1, 0 );
      GTIFKeySet( gtif, GeogEllipsoidGeoKey, TYPE_SHORT, 1,
                  0 );
      GTIFKeySet( gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1,
                  0.0 );
      GTIFKeySet( gtif, GeogSemiMinorAxisGeoKey, TYPE_DOUBLE, 1,
                  0.0 );
      switch(tile->grid_link->grid->unit) {
        case MAPCACHE_UNIT_FEET:
          GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                      Linear_Foot );
          break;
        case MAPCACHE_UNIT_METERS:
          GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                      Linear_Meter );
          break;
        case MAPCACHE_UNIT_DEGREES:
          GTIFKeySet(gtif, GeogAngularUnitsGeoKey, TYPE_SHORT, 0,
                     Angular_Degree );
          break;
        default:
          break;
      }

      GTIFWriteKeys(gtif);
      GTIFFree(gtif);

      adfPixelScale[0] = adfPixelScale[1] = level->resolution;
      adfPixelScale[2] = 0.0;
      TIFFSetField( hTIFF, TIFFTAG_GEOPIXELSCALE, 3, adfPixelScale );


      /* top left tile x,y */
      x = (tile->x / cache->count_x)*(cache->count_x);
      y = (tile->y / cache->count_y)*(cache->count_y) + ntilesy - 1;

      mapcache_grid_get_tile_extent(ctx, tile->grid_link->grid,
                               x,y,tile->z,&bbox);
      adfTiePoints[0] = 0.0;
      adfTiePoints[1] = 0.0;
      adfTiePoints[2] = 0.0;
      adfTiePoints[3] = bbox.minx;
      adfTiePoints[4] = bbox.maxy;
      adfTiePoints[5] = 0.0;
      TIFFSetField( hTIFF, TIFFTAG_GEOTIEPOINTS, 6, adfTiePoints );
    }

#endif
  }
  TIFFSetField(hTIFF, TIFFTAG_JPEGQUALITY, format->quality);
  if(format->photometric == MAPCACHE_PHOTOMETRIC_RGB) {
    TIFFSetField( hTIFF, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  } else {
    TIFFSetField( hTIFF, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
  }
  TIFFSetField( hTIFF, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB );

  /* x offset of the tile along a row */
  tiff_offx = tile->x % ntilesx;

  /*
   * y offset of the requested row. we inverse it as the rows are ordered
   * from top to bottom, whereas the tile y is bottom to top
   */
  tiff_offy = ntilesy - (tile->y % ntilesy) -1;
  tiff_off = tiff_offy * ntilesx + tiff_offx;


  rv = TIFFWriteEncodedTile(hTIFF, tiff_off, rgb, tilew*tileh*3);
  free(rgb);
  if(!rv) {
    ctx->set_error(ctx,500,"failed TIFFWriteEncodedTile to %s",filename);
    goto close_tiff;
  }
  rv = TIFFWriteCheck( hTIFF, 1, "cache_set()");
  if(!rv) {
    ctx->set_error(ctx,500,"failed TIFFWriteCheck %s",filename);
    goto close_tiff;
  }

  if(create) {
    rv = TIFFWriteDirectory(hTIFF);
    if(!rv) {
      ctx->set_error(ctx,500,"failed TIFFWriteDirectory to %s",filename);
      goto close_tiff;
    }
  }

close_tiff:
  if(hTIFF)
    MyTIFFClose(hTIFF);
  mapcache_unlock_resource(ctx,cache->locker?cache->locker:ctx->config->locker, lock);
#else
  ctx->set_error(ctx,500,"tiff write support disabled by default");
#endif

}

/**
 * \private \memberof mapcache_cache_tiff
 */
static void _mapcache_cache_tiff_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *pcache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_tiff *cache = (mapcache_cache_tiff*)pcache;
  char * format_name;
  mapcache_image_format *pformat;
  if ((cur_node = ezxml_child(node,"template")) != NULL) {
    char *fmt;
    cache->filename_template = apr_pstrdup(ctx->pool,cur_node->txt);
    fmt = (char*)ezxml_attr(cur_node,"x_fmt");
    if(fmt && *fmt) {
      cache->x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"y_fmt");
    if(fmt && *fmt) {
      cache->y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"z_fmt");
    if(fmt && *fmt) {
      cache->z_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_x_fmt");
    if(fmt && *fmt) {
      cache->inv_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_y_fmt");
    if(fmt && *fmt) {
      cache->inv_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"div_x_fmt");
    if(fmt && *fmt) {
      cache->div_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"div_y_fmt");
    if(fmt && *fmt) {
      cache->div_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_div_x_fmt");
    if(fmt && *fmt) {
      cache->inv_div_x_fmt = apr_pstrdup(ctx->pool,fmt);
    }
    fmt = (char*)ezxml_attr(cur_node,"inv_div_y_fmt");
    if(fmt && *fmt) {
      cache->inv_div_y_fmt = apr_pstrdup(ctx->pool,fmt);
    }
  }
  cur_node = ezxml_child(node,"xcount");
  if(cur_node && cur_node->txt && *cur_node->txt) {
    char *endptr;
    cache->count_x = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400,"failed to parse xcount value %s for tiff cache %s", cur_node->txt,pcache->name);
      return;
    }
  }
  cur_node = ezxml_child(node,"ycount");
  if(cur_node && cur_node->txt && *cur_node->txt) {
    char *endptr;
    cache->count_y = (int)strtol(cur_node->txt,&endptr,10);
    if(*endptr != 0) {
      ctx->set_error(ctx,400,"failed to parse ycount value %s for tiff cache %s", cur_node->txt,pcache->name);
      return;
    }
  }
  cur_node = ezxml_child(node,"format");
  if(cur_node && cur_node->txt && *cur_node->txt) {
    format_name = cur_node->txt;
  } else {
    format_name = "JPEG";
  }
  pformat = mapcache_configuration_get_image_format(
              config,format_name);
  if(!pformat) {
    ctx->set_error(ctx,500,"TIFF cache %s references unknown image format %s",
                   pcache->name, format_name);
    return;
  }
  if(pformat->type != GC_JPEG) {
    ctx->set_error(ctx,500,"TIFF cache %s can only reference a JPEG image format",
                   pcache->name);
    return;
  }
  cache->format = (mapcache_image_format_jpeg*)pformat;

  cur_node = ezxml_child(node,"locker");
  if(cur_node) {
    mapcache_config_parse_locker(ctx, cur_node, &cache->locker);
  }

  cache->storage.type = MAPCACHE_TIFF_STORAGE_FILE;
  cur_node = ezxml_child(node,"storage");
  if (cur_node) {
    ezxml_t child_node;
    const char *type = ezxml_attr(cur_node,"type");
    if( !type ) {
      ctx->set_error(ctx,400,
                     "<storage> with no \"type\" attribute in cache (%s)",
                     pcache->name);
      return;
    }

    if( strcmp(type, "rest") == 0 ) {
      cache->storage.type = MAPCACHE_TIFF_STORAGE_REST;
    }
    else if( strcmp(type, "google") == 0 ) {
      cache->storage.type = MAPCACHE_TIFF_STORAGE_GOOGLE;
      if ((child_node = ezxml_child(cur_node,"access")) != NULL) {
        cache->storage.u.google.access =
            apr_pstrdup(ctx->pool, child_node->txt);
      } else if ( getenv("GS_ACCESS_KEY_ID")) {
        cache->storage.u.google.access =
            apr_pstrdup(ctx->pool,getenv("GS_ACCESS_KEY_ID"));
      } else {
        ctx->set_error(ctx,400,
                       "google storage in cache (%s) is missing "
                       "required <access> child", pcache->name);
        return;
      }
      if ((child_node = ezxml_child(cur_node,"secret")) != NULL) {
        cache->storage.u.google.secret =
            apr_pstrdup(ctx->pool, child_node->txt);
      } else if ( getenv("GS_SECRET_ACCESS_KEY")) {
        cache->storage.u.google.access =
            apr_pstrdup(ctx->pool,getenv("GS_SECRET_ACCESS_KEY"));
      } else {
        ctx->set_error(ctx,400,
                       "google storage in cache (%s) is missing "
                       "required <secret> child", pcache->name);
        return;
      }
    }
    else {
      ctx->set_error(ctx, 400, "unknown cache type %s for cache \"%s\"",
                     type, pcache->name);
      return;
    }

    if ((child_node = ezxml_child(cur_node,"connection_timeout")) != NULL) {
      char *endptr;
      cache->storage.connection_timeout = (int)strtol(child_node->txt,&endptr,10);
      if(*endptr != 0 || cache->storage.connection_timeout<1) {
        ctx->set_error(ctx,400,"invalid rest cache <connection_timeout> "
                       "\"%s\" (positive integer expected)",
                       child_node->txt);
        return;
      }
    } else {
      cache->storage.connection_timeout = 30;
    }

    if ((child_node = ezxml_child(cur_node,"timeout")) != NULL) {
      char *endptr;
      cache->storage.timeout = (int)strtol(child_node->txt,&endptr,10);
      if(*endptr != 0 || cache->storage.timeout<1) {
        ctx->set_error(ctx,400,"invalid rest cache <timeout> \"%s\" "
                       "(positive integer expected)",
                       child_node->txt);
        return;
      }
    } else {
      cache->storage.timeout = 120;
    }

    if ((child_node = ezxml_child(cur_node,"header_file")) != NULL) {
      cache->storage.header_file = apr_pstrdup(ctx->pool, child_node->txt);
    }

  }

}

/**
 * \private \memberof mapcache_cache_tiff
 */
static void _mapcache_cache_tiff_configuration_post_config(mapcache_context *ctx, mapcache_cache *pcache,
    mapcache_cfg *cfg)
{
  mapcache_cache_tiff *cache = (mapcache_cache_tiff*)pcache;
  /* check all required parameters are configured */
  if((!cache->filename_template || !strlen(cache->filename_template))) {
    ctx->set_error(ctx, 400, "tiff cache %s has no template pattern",cache->cache.name);
    return;
  }
  if(cache->count_x <= 0 || cache->count_y <= 0) {
    ctx->set_error(ctx, 400, "tiff cache %s has invalid count (%d,%d)",cache->count_x,cache->count_y);
    return;
  }

#ifdef USE_GDAL
  if(cache->storage.type == MAPCACHE_TIFF_STORAGE_REST &&
     strncmp(cache->filename_template, "http://", 6) != 0 &&
     strncmp(cache->filename_template, "https://", 7) != 0 ) {
    ctx->set_error(ctx, 400, "tiff cache %s template pattern should begin with http:// or https://",cache->cache.name);
    return;
  }

  if(cache->storage.type == MAPCACHE_TIFF_STORAGE_GOOGLE &&
     strncmp(cache->filename_template, "https://storage.googleapis.com/",
             strlen("https://storage.googleapis.com/")) != 0 &&
     strncmp(cache->filename_template, "/vsigs/",
             strlen("/vsigs/")) != 0 ) {
    ctx->set_error(ctx, 400, "tiff cache %s template pattern should begin "
                   "with https://storage.googleapis.com/ or /vsigs/",cache->cache.name);
    return;
  }
#else
  if(cache->storage.type != MAPCACHE_TIFF_STORAGE_FILE )
  {
    ctx->set_error(ctx, 400, "tiff cache %s cannot use a network based "
                   "storage due to mising GDAL dependency",
                   cache->cache.name);
    return;
  }
#endif
}

/**
 * \brief creates and initializes a mapcache_tiff_cache
 */
mapcache_cache* mapcache_cache_tiff_create(mapcache_context *ctx)
{
  mapcache_cache_tiff *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_tiff));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate tiff cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_TIFF;
  cache->cache._tile_delete = _mapcache_cache_tiff_delete;
  cache->cache._tile_get = _mapcache_cache_tiff_get;
  cache->cache._tile_exists = _mapcache_cache_tiff_has_tile;
  cache->cache._tile_set = _mapcache_cache_tiff_set;
  cache->cache.configuration_post_config = _mapcache_cache_tiff_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_tiff_configuration_parse_xml;
  cache->count_x = 10;
  cache->count_y = 10;
  cache->x_fmt = cache->y_fmt = cache->z_fmt
                                = cache->inv_x_fmt = cache->inv_y_fmt
                                    = cache->div_x_fmt = cache->div_y_fmt
                                        = cache->inv_div_x_fmt = cache->inv_div_y_fmt = apr_pstrdup(ctx->pool,"%d");
#ifndef DEBUG
  TIFFSetWarningHandler(NULL);
  TIFFSetErrorHandler(NULL);
#endif
  return (mapcache_cache*)cache;
}

#else

mapcache_cache* mapcache_cache_tiff_create(mapcache_context *ctx) {
  ctx->set_error(ctx,400,"TIFF support not compiled in this version");
  return NULL;
}

#endif

/* vim: ts=2 sts=2 et sw=2
*/
