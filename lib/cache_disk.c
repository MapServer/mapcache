/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching: filesytem cache backend.
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
#include <apr_file_info.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <string.h>
#include <errno.h>
#include <apr_mmap.h>

#ifdef HAVE_SYMLINK
#include <unistd.h>
#endif

/**\class mapcache_cache_disk
 * \brief a mapcache_cache on a filesytem
 * \implements mapcache_cache
 */
typedef struct mapcache_cache_disk mapcache_cache_disk;
struct mapcache_cache_disk {
  mapcache_cache cache;
  char *base_directory;
  char *filename_template;
  int symlink_blank;
  int detect_blank;
  int creation_retry;

  /**
   * Set filename for a given tile
   * \memberof mapcache_cache_disk
   */
  void (*tile_key)(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path);
};


/**
 * \brief computes the relative path between two destinations
 *
 * \param tilename the absolute filename of the tile
 * \param blankname the absolute path of the blank tile image
 */

char* relative_path(mapcache_context *ctx, char* tilename, char* blankname)
{
  int updir_cnt = 0;
  char *blank_rel = "";

  /* work up the directory paths of the tile and blank filename to find the common
   root */
  char *tile_it = tilename, *blank_it = blankname;
  if(*tile_it != *blank_it) {
    /* the two files have no common root.
     * This really shouldn't happen on a unix FS hierarchy, and symbolic linking
     * is enabled only on these platforms, so this case should in practice never
     * happen.
     * we return the absolute path, and should probably set a warning message
     */
    return apr_pstrdup(ctx->pool, blankname);
  }
  while(*(tile_it+1) && *(blank_it+1) && *(tile_it+1) == *(blank_it+1)) {
    tile_it++;
    blank_it++;
  }

  /* tile_it and blank_it point on the last common character of the two filenames,
   which should be a '/'. If not, return the full blank name
   * (and set a warning message? )*/
  if(*tile_it != *blank_it || *tile_it != '/') {
    return apr_pstrdup(ctx->pool, blankname);
  }

  blank_it++;
  while(*tile_it == '/') tile_it++; /*skip leading '/'s*/

  /* blank_it now contains the path that must be appended after the relative
   part of the constructed path,e.g.:
     - tilename = "/basepath/tilesetname/gridname/03/000/05/08.png"
     - blankname = "/basepath/tilesetname/gridname/blanks/005599FF.png"
   then
     - tile_it is "03/000/05/08.png"
     - blank_it is "blanks/005599FF.png"
   */

  /* we now count the number of '/' in the remaining tilename */
  while(*tile_it) {
    if(*tile_it == '/') {
      updir_cnt++;
      /* also skip consecutive '/'s */
      while(*(tile_it+1)=='/') tile_it++;
    }
    tile_it ++;
  }

  while(updir_cnt--) {
    blank_rel = apr_pstrcat(ctx->pool, blank_rel, "../", NULL);
  }
  blank_rel = apr_pstrcat(ctx->pool,blank_rel,blank_it,NULL);
  return blank_rel;
}

/**
 * \brief returns base path for given tile
 *
 * \param tile the tile to get base path from
 * \param path pointer to a char* that will contain the filename
 * \private \memberof mapcache_cache_disk
 */
static void _mapcache_cache_disk_base_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path)
{
  *path = apr_pstrcat(ctx->pool,
                      cache->base_directory,"/",
                      tile->tileset->name,"/",
                      tile->grid_link->grid->name,
                      NULL);
  if(tile->dimensions) {
    int i = tile->dimensions->nelts;
    while(i--) {
      mapcache_requested_dimension *entry = APR_ARRAY_IDX(tile->dimensions,i,mapcache_requested_dimension*);
      char *dimval;
      if(!entry->cached_value) {
        ctx->set_error(ctx,500,"BUG: dimension (%s) not set",entry->dimension->name);
        return;
      }
      dimval = mapcache_util_str_sanitize(ctx->pool,entry->cached_value,"/.",'#');
      *path = apr_pstrcat(ctx->pool,*path,"/",dimval,NULL);
    }
  }
}

static void _mapcache_cache_disk_blank_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, unsigned char *color, char **path)
{
  /* not implemented for template caches, as symlink_blank will never be set */
  *path = apr_psprintf(ctx->pool,"%s/%s/%s/blanks/%02X%02X%02X%02X.%s",
                       cache->base_directory,
                       tile->tileset->name,
                       tile->grid_link->grid->name,
                       color[0],
                       color[1],
                       color[2],
                       color[3],
                       tile->tileset->format?tile->tileset->format->extension:"png");
  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate blank tile key");
  }
}

/**
 * \brief return filename for given tile
 *
 * \param tile the tile to get the key from
 * \param path pointer to a char* that will contain the filename
 * \param r
 * \private \memberof mapcache_cache_disk
 */
static void _mapcache_cache_disk_tilecache_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path)
{
  if(cache->base_directory) {
    char *start;
    _mapcache_cache_disk_base_tile_key(ctx, cache, tile, &start);
    *path = apr_psprintf(ctx->pool,"%s/%02d/%03d/%03d/%03d/%03d/%03d/%03d.%s",
                         start,
                         tile->z,
                         tile->x / 1000000,
                         (tile->x / 1000) % 1000,
                         tile->x % 1000,
                         tile->y / 1000000,
                         (tile->y / 1000) % 1000,
                         tile->y % 1000,
                         tile->tileset->format?tile->tileset->format->extension:"png");
  } else {
    *path = cache->filename_template;
    *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}", tile->tileset->name);
    *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}", tile->grid_link->grid->name);
    *path = mapcache_util_str_replace(ctx->pool,*path, "{ext}",
                                      tile->tileset->format?tile->tileset->format->extension:"png");
    if(strstr(*path,"{x}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                                        apr_psprintf(ctx->pool,"%d",tile->x));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1));
    if(strstr(*path,"{y}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                                        apr_psprintf(ctx->pool,"%d",tile->y));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1));
    if(strstr(*path,"{z}"))
      *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
                                        apr_psprintf(ctx->pool,"%d",tile->z));
    else
      *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_z}",
                                        apr_psprintf(ctx->pool,"%d",
                                            tile->grid_link->grid->nlevels - tile->z - 1));
    if(tile->dimensions && strstr(*path,"{dim")) {
      char *dimstring="";
      int i = tile->dimensions->nelts;
      while(i--) {
        mapcache_requested_dimension *entry = APR_ARRAY_IDX(tile->dimensions,i, mapcache_requested_dimension*);
        char *dimval;
        char *single_dim;
        char *iter;
        if(!entry->cached_value) {
          ctx->set_error(ctx,500,"BUG: dimension (%s) not set",entry->dimension->name);
          return;
        }
        dimval = apr_pstrdup(ctx->pool,entry->cached_value);
        iter = dimval;
        while(*iter) {
          /* replace dangerous characters by '#' */
          if(*iter == '.' || *iter == '/') {
            *iter = '#';
          }
          iter++;
        }
        dimstring = apr_pstrcat(ctx->pool,dimstring,"#",entry->dimension->name,"#",dimval,NULL);
        single_dim = apr_pstrcat(ctx->pool,"{dim:",entry->dimension->name,"}",NULL);
        if(strstr(*path,single_dim)) {
          *path = mapcache_util_str_replace(ctx->pool,*path, single_dim, dimval);
        }
      }
      *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
    }
  }
  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

static void _mapcache_cache_disk_template_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path)
{

  *path = cache->filename_template;
  *path = mapcache_util_str_replace(ctx->pool,*path, "{tileset}", tile->tileset->name);
  *path = mapcache_util_str_replace(ctx->pool,*path, "{grid}", tile->grid_link->grid->name);
  *path = mapcache_util_str_replace(ctx->pool,*path, "{ext}",
                                    tile->tileset->format?tile->tileset->format->extension:"png");

  if(strstr(*path,"{x}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{x}",
                                      apr_psprintf(ctx->pool,"%d",tile->x));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_x}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->levels[tile->z]->maxx - tile->x - 1));
  if(strstr(*path,"{y}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{y}",
                                      apr_psprintf(ctx->pool,"%d",tile->y));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_y}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->levels[tile->z]->maxy - tile->y - 1));
  if(strstr(*path,"{z}"))
    *path = mapcache_util_str_replace(ctx->pool,*path, "{z}",
                                      apr_psprintf(ctx->pool,"%d",tile->z));
  else
    *path = mapcache_util_str_replace(ctx->pool,*path, "{inv_z}",
                                      apr_psprintf(ctx->pool,"%d",
                                          tile->grid_link->grid->nlevels - tile->z - 1));
  if(tile->dimensions && strstr(*path,"{dim")) {
    char *dimstring="";
    int i = tile->dimensions->nelts;
    while(i--) {
      mapcache_requested_dimension *entry = APR_ARRAY_IDX(tile->dimensions,i, mapcache_requested_dimension*);
      char *dimval;
      char *single_dim;
      char *iter;
      if(!entry->cached_value) {
        ctx->set_error(ctx,500,"BUG: dimension (%s) not set",entry->dimension->name);
        return;
      }
      dimval = apr_pstrdup(ctx->pool,entry->cached_value);
      iter = dimval;
      while(*iter) {
        /* replace dangerous characters by '#' */
        if(*iter == '.' || *iter == '/') {
          *iter = '#';
        }
        iter++;
      }
      dimstring = apr_pstrcat(ctx->pool,dimstring,"#",entry->dimension->name,"#",dimval,NULL);
      single_dim = apr_pstrcat(ctx->pool,"{dim:",entry->dimension->name,"}",NULL);
      if(strstr(*path,single_dim)) {
        *path = mapcache_util_str_replace(ctx->pool,*path, single_dim, dimval);
      }
    }
    *path = mapcache_util_str_replace(ctx->pool,*path, "{dim}", dimstring);
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

static void _mapcache_cache_disk_arcgis_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path)
{
  if(cache->base_directory) {
    char *start;
    _mapcache_cache_disk_base_tile_key(ctx, cache, tile, &start);
    *path = apr_psprintf(ctx->pool,"%s/L%02d/R%08x/C%08x.%s" ,
                         start,
                         tile->z,
                         tile->y,
                         tile->x,
                         tile->tileset->format?tile->tileset->format->extension:"png");
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}

static void _mapcache_cache_disk_worldwind_tile_key(mapcache_context *ctx, mapcache_cache_disk *cache, mapcache_tile *tile, char **path)
{
  if(cache->base_directory) {
    *path = apr_psprintf(ctx->pool,"%s/%d/%04d/%04d_%04d.%s" ,
                         cache->base_directory,
                         tile->z,
                         tile->y,
                         tile->y,
                         tile->x,
                         tile->tileset->format?tile->tileset->format->extension:"png");
  }

  if(!*path) {
    ctx->set_error(ctx,500, "failed to allocate tile key");
  }
}


static int _mapcache_cache_disk_has_tile(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *filename;
  apr_finfo_t finfo;
  int rv;
  mapcache_cache_disk *cache = (mapcache_cache_disk*)pcache;
  cache->tile_key(ctx, cache, tile, &filename);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FALSE;
  }
  rv = apr_stat(&finfo,filename,0,ctx->pool);
  if(rv != APR_SUCCESS) {
    return MAPCACHE_FALSE;
  } else {
    return MAPCACHE_TRUE;
  }
}

static void _mapcache_cache_disk_delete(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  apr_status_t ret;
  char errmsg[120];
  char *filename;
  mapcache_cache_disk *cache = (mapcache_cache_disk*)pcache;
  cache->tile_key(ctx, cache, tile, &filename);
  GC_CHECK_ERROR(ctx);

  ret = apr_file_remove(filename,ctx->pool);
  if(ret != APR_SUCCESS && !APR_STATUS_IS_ENOENT(ret)) {
    ctx->set_error(ctx, 500,  "failed to remove file %s: %s",filename, apr_strerror(ret,errmsg,120));
  }
}


/**
 * \brief get file content of given tile
 *
 * fills the mapcache_tile::data of the given tile with content stored in the file
 * \private \memberof mapcache_cache_disk
 * \sa mapcache_cache::tile_get()
 */
static int _mapcache_cache_disk_get(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  char *filename;
  apr_file_t *f;
  apr_finfo_t finfo;
  apr_status_t rv;
  apr_size_t size;
  apr_mmap_t *tilemmap;
  mapcache_cache_disk *cache = (mapcache_cache_disk*)pcache;

  cache->tile_key(ctx, cache, tile, &filename);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FAILURE;
  }
  ctx->log(ctx,MAPCACHE_DEBUG,"checking for tile %s",filename);
  if((rv=apr_file_open(&f, filename,
#ifndef NOMMAP
                       APR_FOPEN_READ, APR_UREAD | APR_GREAD,
#else
                       APR_FOPEN_READ|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,APR_OS_DEFAULT,
#endif
                       ctx->pool)) == APR_SUCCESS) {
    rv = apr_file_info_get(&finfo, APR_FINFO_SIZE|APR_FINFO_MTIME, f);
    if(!finfo.size) {
      ctx->log(ctx, MAPCACHE_WARN, "tile %s has 0 length data",filename);
      return MAPCACHE_CACHE_MISS;
    }

    size = finfo.size;
    /*
     * at this stage, we have a handle to an open file that contains data.
     * idealy, we should aquire a read lock, in case the data contained inside the file
     * is incomplete (i.e. if another process is currently writing to the tile).
     * currently such a lock is not set, as we don't want to loose performance on tile accesses.
     * any error that might happen at this stage should only occur if the tile isn't already cached,
     * i.e. normally only once.
     */
    tile->mtime = finfo.mtime;

#ifndef NOMMAP

    tile->encoded_data = mapcache_buffer_create(0,ctx->pool);
    rv = apr_mmap_create(&tilemmap,f,0,finfo.size,APR_MMAP_READ,ctx->pool);
    if(rv != APR_SUCCESS) {
      char errmsg[120];
      ctx->set_error(ctx, 500,  "mmap error: %s",apr_strerror(rv,errmsg,120));
      return MAPCACHE_FAILURE;
    }
    tile->encoded_data->buf = tilemmap->mm;
    tile->encoded_data->size = tile->encoded_data->avail = finfo.size;
#else
    tile->encoded_data = mapcache_buffer_create(size,ctx->pool);
    //manually add the data to our buffer
    apr_file_read(f,(void*)tile->encoded_data->buf,&size);
    tile->encoded_data->size = size;
    tile->encoded_data->avail = size;
#endif
    apr_file_close(f);
    if(tile->encoded_data->size != finfo.size) {
      ctx->set_error(ctx, 500,  "failed to copy image data, got %d of %d bytes",(int)size, (int)finfo.size);
      return MAPCACHE_FAILURE;
    }
    return MAPCACHE_SUCCESS;
  } else {
    if(APR_STATUS_IS_ENOENT(rv)) {
      /* the file doesn't exist on the disk */
      return MAPCACHE_CACHE_MISS;
    } else {
      char *error = strerror(rv);
      ctx->set_error(ctx, 500,  "failed to open file %s: %s",filename, error);
      return MAPCACHE_FAILURE;
    }
  }
}

/**
 * \brief write tile data to disk
 *
 * writes the content of mapcache_tile::data to disk.
 * \returns MAPCACHE_FAILURE if there is no data to write, or if the tile isn't locked
 * \returns MAPCACHE_SUCCESS if the tile has been successfully written to disk
 * \private \memberof mapcache_cache_disk
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_disk_set(mapcache_context *ctx, mapcache_cache *pcache, mapcache_tile *tile)
{
  apr_size_t bytes;
  apr_file_t *f;
  apr_status_t ret;
  char errmsg[120];
  char *filename;
  mapcache_cache_disk *cache = (mapcache_cache_disk*)pcache;
  const int creation_retry = cache->creation_retry;
  int retry_count_create_file = 0;

#ifdef DEBUG
  /* all this should be checked at a higher level */
  if(!tile->encoded_data && !tile->raw_image) {
    ctx->set_error(ctx,500,"attempting to write empty tile to disk");
    return;
  }
  if(!tile->encoded_data && !tile->tileset->format) {
    ctx->set_error(ctx,500,"received a raw tile image for a tileset with no format");
    return;
  }
#endif

  cache->tile_key(ctx, cache, tile, &filename);
  GC_CHECK_ERROR(ctx);
  if ( cache->detect_blank ) {
    if(!tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    if(mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      if(tile->raw_image->data[3] == 0) {
        /* We have a blank (uniform) image who's first pixel is fully transparent, thus the whole image is transparent */
#ifdef DEBUG
        ctx->log(ctx, MAPCACHE_DEBUG, "skipped blank tile %s",filename);
#endif
        tile->nodata = 1;
        return;
      }
    }
  }

  mapcache_make_parent_dirs(ctx,filename);
  GC_CHECK_ERROR(ctx);

  ret = apr_file_remove(filename,ctx->pool);
  if(ret != APR_SUCCESS && !APR_STATUS_IS_ENOENT(ret)) {
    ctx->set_error(ctx, 500,  "failed to remove file %s: %s",filename, apr_strerror(ret,errmsg,120));
  }

#ifdef HAVE_SYMLINK
  if(cache->symlink_blank) {
    if(tile->tileset->format->type != GC_RAW && !tile->raw_image) {
      tile->raw_image = mapcache_imageio_decode(ctx, tile->encoded_data);
      GC_CHECK_ERROR(ctx);
    }
    if(tile->tileset->format->type != GC_RAW && mapcache_image_blank_color(tile->raw_image) != MAPCACHE_FALSE) {
      char *blankname;
      int retry_count_create_symlink = 0;
      char *blankname_rel = NULL;
      _mapcache_cache_disk_blank_tile_key(ctx,cache,tile,tile->raw_image->data,&blankname);
      if(apr_file_open(&f, blankname, APR_FOPEN_READ, APR_OS_DEFAULT, ctx->pool) != APR_SUCCESS) {
        /* create the blank file */
        int isLocked;
        void *lock;
        if(!tile->encoded_data) {
          tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
          GC_CHECK_ERROR(ctx);
        }
        mapcache_make_parent_dirs(ctx,blankname);
        GC_CHECK_ERROR(ctx);

        /* aquire a lock on the blank file */
        isLocked = mapcache_lock_or_wait_for_resource(ctx,ctx->config->locker,blankname, &lock);

        if(isLocked == MAPCACHE_TRUE) {

          if((ret = apr_file_open(&f, blankname,
                                  APR_FOPEN_CREATE|APR_FOPEN_WRITE|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,
                                  APR_OS_DEFAULT, ctx->pool)) != APR_SUCCESS) {
            ctx->set_error(ctx, 500,  "failed to create file %s: %s",blankname, apr_strerror(ret,errmsg,120));
            mapcache_unlock_resource(ctx,ctx->config->locker, lock);
            return; /* we could not create the file */
          }

          bytes = (apr_size_t)tile->encoded_data->size;
          ret = apr_file_write(f,(void*)tile->encoded_data->buf,&bytes);
          if(ret != APR_SUCCESS) {
            ctx->set_error(ctx, 500,  "failed to write data to file %s (wrote %d of %d bytes): %s",blankname, (int)bytes, (int)tile->encoded_data->size, apr_strerror(ret,errmsg,120));
            mapcache_unlock_resource(ctx,ctx->config->locker, lock);
            return; /* we could not create the file */
          }

          if(bytes != tile->encoded_data->size) {
            ctx->set_error(ctx, 500,  "failed to write image data to %s, wrote %d of %d bytes", blankname, (int)bytes, (int)tile->encoded_data->size);
            mapcache_unlock_resource(ctx,ctx->config->locker, lock);
            return;
          }
          apr_file_close(f);
          mapcache_unlock_resource(ctx,ctx->config->locker, lock);
#ifdef DEBUG
          ctx->log(ctx,MAPCACHE_DEBUG,"created blank tile %s",blankname);
#endif
        }
      } else {
        apr_file_close(f);
      }


      /*
       * compute the relative path between tile and blank tile
       */
      blankname_rel = relative_path(ctx,filename, blankname);
      GC_CHECK_ERROR(ctx);

      /*
       * depending on configuration symlink creation will retry if it fails.
       * this can happen on nfs mounted network storage.
       * the solution is to create the containing directory again and retry the symlink creation.
       */
      while(symlink(blankname_rel, filename) != 0) {
        retry_count_create_symlink++;

        if(retry_count_create_symlink > creation_retry) {
          char *error = strerror(errno);
          ctx->set_error(ctx, 500, "failed to link tile %s to %s: %s",filename, blankname_rel, error);
          return; /* we could not create the file */
        }
        mapcache_make_parent_dirs(ctx,filename);
        GC_CHECK_ERROR(ctx);
      }
#ifdef DEBUG
      ctx->log(ctx, MAPCACHE_DEBUG, "linked blank tile %s to %s",filename,blankname);
#endif
      return;
    }
  }
#endif /* HAVE_SYMLINK */

  /* go the normal way: either we haven't configured blank tile detection, or the tile was not blank */

  if(!tile->encoded_data) {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }

  bytes = (apr_size_t)tile->encoded_data->size;
  if(bytes == 0) {
      ctx->set_error(ctx, 500, "attempting to write 0 length tile to %s",filename);
      return; /* we could not create the file */
  }

  /*
   * depending on configuration file creation will retry if it fails.
   * this can happen on nfs mounted network storage.
   * the solution is to create the containing directory again and retry the file creation.
   */
  while((ret = apr_file_open(&f, filename,
                             APR_FOPEN_CREATE|APR_FOPEN_WRITE|APR_FOPEN_BUFFERED|APR_FOPEN_BINARY,
                             APR_OS_DEFAULT, ctx->pool)) != APR_SUCCESS) {

    retry_count_create_file++;

    if(retry_count_create_file > creation_retry) {
      ctx->set_error(ctx, 500, "failed to create file %s: %s",filename, apr_strerror(ret,errmsg,120));
      return; /* we could not create the file */
    }
    mapcache_make_parent_dirs(ctx,filename);
    GC_CHECK_ERROR(ctx);
  }

  ret = apr_file_write(f,(void*)tile->encoded_data->buf,&bytes);
  if(ret != APR_SUCCESS) {
    ctx->set_error(ctx, 500,  "failed to write data to file %s (wrote %d of %d bytes): %s",filename, (int)bytes, (int)tile->encoded_data->size, apr_strerror(ret,errmsg,120));
    return; /* we could not create the file */
  }

  ret = apr_file_close(f);
  if(ret != APR_SUCCESS) {
    ctx->set_error(ctx, 500,  "failed to close file %s:%s",filename, apr_strerror(ret,errmsg,120));
  }

  if(bytes != tile->encoded_data->size) {
    ctx->set_error(ctx, 500, "failed to write image data to %s, wrote %d of %d bytes", filename, (int)bytes, (int)tile->encoded_data->size);
    apr_file_remove(filename, ctx->pool);
  }

}

/**
 * \private \memberof mapcache_cache_disk
 */
static void _mapcache_cache_disk_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config)
{
  ezxml_t cur_node;
  mapcache_cache_disk *dcache = (mapcache_cache_disk*)cache;
  char *layout = NULL;
  int template_layout = MAPCACHE_FALSE;

  layout = (char*)ezxml_attr(node,"layout");
  if (!layout || !strlen(layout) || !strcmp(layout,"tilecache")) {
    dcache->tile_key = _mapcache_cache_disk_tilecache_tile_key;
  } else if(!strcmp(layout,"arcgis")) {
    dcache->tile_key = _mapcache_cache_disk_arcgis_tile_key;
  } else if(!strcmp(layout,"worldwind")) {
    dcache->tile_key = _mapcache_cache_disk_worldwind_tile_key;
  } else if (!strcmp(layout,"template")) {
    dcache->tile_key = _mapcache_cache_disk_template_tile_key;
    template_layout = MAPCACHE_TRUE;
    if ((cur_node = ezxml_child(node,"template")) != NULL) {
      dcache->filename_template = apr_pstrdup(ctx->pool,cur_node->txt);
    } else {
      ctx->set_error(ctx, 400, "no template specified for cache \"%s\"", cache->name);
      return;
    }
  } else {
    ctx->set_error(ctx, 400, "unknown layout type %s for cache \"%s\"", layout, cache->name);
    return;
  }

  if (!template_layout && (cur_node = ezxml_child(node,"base")) != NULL) {
    dcache->base_directory = apr_pstrdup(ctx->pool,cur_node->txt);
  }

  if (!template_layout && (cur_node = ezxml_child(node,"symlink_blank")) != NULL) {
    if(strcasecmp(cur_node->txt,"false")) {
#ifdef HAVE_SYMLINK
      dcache->symlink_blank=1;
#else
      ctx->set_error(ctx,400,"cache %s: host system does not support file symbolic linking",cache->name);
      return;
#endif
    }
  }

  if ((cur_node = ezxml_child(node,"creation_retry")) != NULL) {
    dcache->creation_retry = atoi(cur_node->txt);
  }
  if ((cur_node = ezxml_child(node,"detect_blank")) != NULL) {
    dcache->detect_blank=1;
  }

}

/**
 * \private \memberof mapcache_cache_disk
 */
static void _mapcache_cache_disk_configuration_post_config(mapcache_context *ctx, mapcache_cache *cache,
    mapcache_cfg *cfg)
{
  mapcache_cache_disk *dcache = (mapcache_cache_disk*)cache;
  /* check all required parameters are configured */
  if((!dcache->base_directory || !strlen(dcache->base_directory)) &&
      (!dcache->filename_template || !strlen(dcache->filename_template))) {
    ctx->set_error(ctx, 400, "disk cache %s has no base directory or template",dcache->cache.name);
    return;
  }
}

/**
 * \brief creates and initializes a mapcache_disk_cache
 */
mapcache_cache* mapcache_cache_disk_create(mapcache_context *ctx)
{
  mapcache_cache_disk *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_disk));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate disk cache");
    return NULL;
  }
  cache->symlink_blank = 0;
  cache->detect_blank = 0;
  cache->creation_retry = 0;
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_DISK;
  cache->cache._tile_delete = _mapcache_cache_disk_delete;
  cache->cache._tile_get = _mapcache_cache_disk_get;
  cache->cache._tile_exists = _mapcache_cache_disk_has_tile;
  cache->cache._tile_set = _mapcache_cache_disk_set;
  cache->cache.configuration_post_config = _mapcache_cache_disk_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_disk_configuration_parse_xml;
  return (mapcache_cache*)cache;
}

/* vim: ts=2 sts=2 et sw=2
*/
