/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: pixel manipulation operations
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
#ifdef USE_PIXMAN
#include <pixman.h>
#else
#include <math.h>
#endif

mapcache_image* mapcache_image_create(mapcache_context *ctx)
{
  mapcache_image *img = (mapcache_image*)apr_pcalloc(ctx->pool,sizeof(mapcache_image));
  img->w= img->h= 0;
  img->data=NULL;
  img->has_alpha = MC_ALPHA_UNKNOWN;
  img->is_blank = MC_EMPTY_UNKNOWN;
  return img;
}

mapcache_image* mapcache_image_create_with_data(mapcache_context *ctx, int width, int height) {
  mapcache_image *img = (mapcache_image*)apr_pcalloc(ctx->pool,sizeof(mapcache_image));
  img->w = width;
  img->h = height;
  img->data = calloc(1, width*height*4*sizeof(unsigned char));
  apr_pool_cleanup_register(ctx->pool, img->data, (void*)free, apr_pool_cleanup_null) ;
  img->stride = 4 * width;
  img->has_alpha = MC_ALPHA_UNKNOWN;
  img->is_blank = MC_EMPTY_UNKNOWN;
  return img;
}

int mapcache_image_has_alpha(mapcache_image *img)
{
  size_t i,j;
  if(img->has_alpha == MC_ALPHA_UNKNOWN) {
    unsigned char *ptr, *rptr = img->data;
    for(i=0; i<img->h; i++) {
      ptr = rptr;
      for(j=0; j<img->w; j++) {
        if(ptr[3]<(unsigned char)255) {
          img->has_alpha = MC_ALPHA_YES;
          return 1;
        }
        ptr += 4;
      }
      rptr += img->stride;
    }
    img->has_alpha = MC_ALPHA_NO;
  }
  assert(img->has_alpha != MC_ALPHA_UNKNOWN);
  if(img->has_alpha == MC_ALPHA_YES) {
    return 1;
  } else {
    return 0;
  }
}

void mapcache_image_merge(mapcache_context *ctx, mapcache_image *base, mapcache_image *overlay)
{
  int starti,startj;
#ifndef USE_PIXMAN
  int i,j;
  unsigned char *browptr, *orowptr, *bptr, *optr;
#endif

  if(base->w < overlay->w || base->h < overlay->h) {
    ctx->set_error(ctx, 500, "attempting to merge an larger image onto another");
    return;
  }
  starti = (base->h - overlay->h)/2;
  startj = (base->w - overlay->w)/2;
#ifdef USE_PIXMAN
  pixman_image_t *si = pixman_image_create_bits(PIXMAN_a8r8g8b8,overlay->w,overlay->h,
                       (uint32_t*)overlay->data,overlay->stride);
  pixman_image_t *bi = pixman_image_create_bits(PIXMAN_a8r8g8b8,base->w,base->h,
                       (uint32_t*)base->data,base->stride);
  pixman_transform_t transform;
  pixman_transform_init_translate(&transform,
                                  pixman_int_to_fixed(-startj),
                                  pixman_int_to_fixed(-starti));
  pixman_image_set_filter(si,PIXMAN_FILTER_NEAREST, NULL, 0);
  pixman_image_set_transform (si, &transform);
  pixman_image_composite (PIXMAN_OP_OVER, si, si, bi,
                          0, 0, 0, 0, 0, 0, base->w,base->h);
  pixman_image_unref(si);
  pixman_image_unref(bi);
#else


  browptr = base->data + starti * base->stride + startj*4;
  orowptr = overlay->data;
  for(i=0; i<overlay->h; i++) {
    bptr = browptr;
    optr = orowptr;
    for(j=0; j<overlay->w; j++) {
      if(optr[3]) { /* if overlay is not completely transparent */
        if(optr[3] == 255) {
          bptr[0]=optr[0];
          bptr[1]=optr[1];
          bptr[2]=optr[2];
          bptr[3]=optr[3];
        } else if(optr[3] != 0) {
          unsigned int br = bptr[0];
          unsigned int bg = bptr[1];
          unsigned int bb = bptr[2];
          unsigned int ba = bptr[3];
          unsigned int or = optr[0];
          unsigned int og = optr[1];
          unsigned int ob = optr[2];
          unsigned int oa = optr[3];
          bptr[0] = (unsigned char)(or + (((255-oa)*br)>>8));
          bptr[1] = (unsigned char)(og + (((255-oa)*bg)>>8));
          bptr[2] = (unsigned char)(ob + (((255-oa)*bb)>>8));

          bptr[3] = oa+((ba*(255-oa))>>8);
        }
      }
      bptr+=4;
      optr+=4;
    }
    browptr += base->stride;
    orowptr += overlay->stride;
  }
#endif
}

#ifndef USE_PIXMAN
#ifndef _WIN32
static inline void bilinear_pixel(mapcache_image *img, double x, double y, unsigned char *dst)
{
#else
static __inline void bilinear_pixel(mapcache_image *img, double x, double y, unsigned char *dst)
{
#endif

  int px,py;
  int px1, py1;
  unsigned char *p1, *p2, *p3, *p4;
  float fx, fy, fx1, fy1;
  int w1, w2, w3, w4;
  px = (int)x;
  py = (int)y;

  px1 = (px==(img->w-1))?(px):(px+1);
  py1 = (py==(img->h-1))?(py):(py+1);

  p1 = &img->data[py*img->stride+px*4];
  p2 = &img->data[py*img->stride+px1*4];
  p3 = &img->data[py1*img->stride+px*4];
  p4 = &img->data[py1*img->stride+px1*4];

  // Calculate the weights for each pixel
  fx = x - px;
  fy = y - py;
  fx1 = 1.0f - fx;
  fy1 = 1.0f - fy;

  w1 = fx1 * fy1 * 256.0f;
  w2 = fx  * fy1 * 256.0f;
  w3 = fx1 * fy  * 256.0f;
  w4 = fx  * fy  * 256.0f;

  // Calculate the weighted sum of pixels (for each color channel)
  dst[0] = (p1[0] * w1 + p2[0] * w2 + p3[0] * w3 + p4[0] * w4) >> 8;
  dst[1] = (p1[1] * w1 + p2[1] * w2 + p3[1] * w3 + p4[1] * w4) >> 8;
  dst[2] = (p1[2] * w1 + p2[2] * w2 + p3[2] * w3 + p4[2] * w4) >> 8;
  dst[3] = (p1[3] * w1 + p2[3] * w2 + p3[3] * w3 + p4[3] * w4) >> 8;
}
#endif

void mapcache_image_copy_resampled_nearest(mapcache_context *ctx, mapcache_image *src, mapcache_image *dst,
    double off_x, double off_y, double scale_x, double scale_y)
{
#ifdef USE_PIXMAN
  pixman_image_t *si = pixman_image_create_bits(PIXMAN_a8r8g8b8,src->w,src->h,
                       (uint32_t*)src->data,src->stride);
  pixman_image_t *bi = pixman_image_create_bits(PIXMAN_a8r8g8b8,dst->w,dst->h,
                       (uint32_t*)dst->data,dst->stride);
  pixman_transform_t transform;
  pixman_transform_init_translate(&transform,pixman_double_to_fixed(-off_x),pixman_double_to_fixed(-off_y));
  pixman_transform_scale(&transform,NULL,pixman_double_to_fixed(1.0/scale_x),pixman_double_to_fixed(1.0/scale_y));
  pixman_image_set_transform (si, &transform);
  pixman_image_set_filter(si,PIXMAN_FILTER_NEAREST, NULL, 0);
  pixman_image_composite (PIXMAN_OP_SRC, si, NULL, bi,
                          0, 0, 0, 0, 0, 0, dst->w,dst->h);
  pixman_image_unref(si);
  pixman_image_unref(bi);
#else
  int dstx,dsty;
  unsigned char *dstrowptr = dst->data;
  for(dsty=0; dsty<dst->h; dsty++) {
    int *dstptr = (int*)dstrowptr;
    int srcy = (int)(((dsty-off_y)/scale_y)+0.5);
    if(srcy >= 0 && srcy < src->h) {
      for(dstx=0; dstx<dst->w; dstx++) {
        int srcx = (int)(((dstx-off_x)/scale_x)+0.5);
        if(srcx >= 0 && srcx < src->w) {
          *dstptr = *((int*)&(src->data[srcy*src->stride+srcx*4]));
        }
        dstptr ++;
      }
    }
    dstrowptr += dst->stride;
  }
#endif
}

void mapcache_image_copy_resampled_bilinear(mapcache_context *ctx, mapcache_image *src, mapcache_image *dst,
    double off_x, double off_y, double scale_x, double scale_y, int reflect_edges)
{
#ifdef USE_PIXMAN
  pixman_image_t *si = pixman_image_create_bits(PIXMAN_a8r8g8b8,src->w,src->h,
                       (uint32_t*)src->data,src->stride);
  pixman_image_t *bi = pixman_image_create_bits(PIXMAN_a8r8g8b8,dst->w,dst->h,
                       (uint32_t*)dst->data,dst->stride);
  pixman_transform_t transform;
  pixman_transform_init_translate(&transform,pixman_double_to_fixed(-off_x),pixman_double_to_fixed(-off_y));
  pixman_transform_scale(&transform,NULL,pixman_double_to_fixed(1.0/scale_x),pixman_double_to_fixed(1.0/scale_y));
  pixman_image_set_transform (si, &transform);
  if(reflect_edges) {
    pixman_image_set_repeat (si, PIXMAN_REPEAT_REFLECT);
  }
  pixman_image_set_filter(si,PIXMAN_FILTER_BILINEAR, NULL, 0);
  pixman_image_composite (PIXMAN_OP_OVER, si, NULL, bi,
                          0, 0, 0, 0, 0, 0, dst->w,dst->h);
  pixman_image_unref(si);
  pixman_image_unref(bi);
#else
  int dstx,dsty;
  unsigned char *dstrowptr = dst->data;
  for(dsty=0; dsty<dst->h; dsty++) {
    unsigned char *dstptr = dstrowptr;
    double srcy = (dsty-off_y)/scale_y;
    if(srcy >= 0 && srcy < src->h) {
      for(dstx=0; dstx<dst->w; dstx++) {
        double srcx = (dstx-off_x)/scale_x;
        if(srcx >= 0 && srcx < src->w) {
          bilinear_pixel(src,srcx,srcy,dstptr);
        }
        dstptr += 4;
      }
    }
    dstrowptr += dst->stride;
  }
#endif
}

void mapcache_image_metatile_split(mapcache_context *ctx, mapcache_metatile *mt)
{
  if(mt->map.tileset->format) {
    /* the tileset has a format defined, we will use it to encode the data */
    mapcache_image *tileimg;
    mapcache_image *metatile;
    int i,j;
    int sx,sy;
    if(mt->map.raw_image) {
      metatile = mt->map.raw_image;
    } else {
      metatile = mapcache_imageio_decode(ctx, mt->map.encoded_data);
    }
    if(!metatile) {
      ctx->set_error(ctx, 500, "failed to load image data from metatile");
      return;
    }
    for(i=0; i<mt->metasize_x; i++) {
      for(j=0; j<mt->metasize_y; j++) {
        tileimg = mapcache_image_create(ctx);
        tileimg->w = mt->map.grid_link->grid->tile_sx;
        tileimg->h = mt->map.grid_link->grid->tile_sy;
        tileimg->stride = metatile->stride;
        switch(mt->map.grid_link->grid->origin) {
          case MAPCACHE_GRID_ORIGIN_BOTTOM_LEFT:
            sx = mt->map.tileset->metabuffer + i * tileimg->w;
            sy = mt->map.height - (mt->map.tileset->metabuffer + (j+1) * tileimg->h);
            break;
          case MAPCACHE_GRID_ORIGIN_TOP_LEFT:
            sx = mt->map.tileset->metabuffer + i * tileimg->w;
            sy = mt->map.tileset->metabuffer + j * tileimg->h;
            break;
          case MAPCACHE_GRID_ORIGIN_BOTTOM_RIGHT: /* FIXME not implemented */
            sx = mt->map.tileset->metabuffer + i * tileimg->w;
            sy = mt->map.height - (mt->map.tileset->metabuffer + (j+1) * tileimg->h);
            break;
          case MAPCACHE_GRID_ORIGIN_TOP_RIGHT:  /* FIXME not implemented */
            sx = mt->map.tileset->metabuffer + i * tileimg->w;
            sy = mt->map.height - (mt->map.tileset->metabuffer + (j+1) * tileimg->h);
            break;
        }
        tileimg->data = &(metatile->data[sy*metatile->stride + 4 * sx]);
        if(mt->map.tileset->watermark) {
          mapcache_image_merge(ctx,tileimg,mt->map.tileset->watermark);
          GC_CHECK_ERROR(ctx);
        }
        mt->tiles[i*mt->metasize_y+j].raw_image = tileimg;
        GC_CHECK_ERROR(ctx);
      }
    }
  } else {
#ifdef DEBUG
    if(mt->map.tileset->metasize_x != 1 ||
        mt->map.tileset->metasize_y != 1 ||
        mt->map.tileset->metabuffer != 0 ||
        !mt->map.encoded_data) {
      ctx->set_error(ctx, 500, "##### BUG ##### using a metatile with no format");
      return;
    }
#endif
    mt->tiles[0].encoded_data = mt->map.encoded_data;
  }
}

int mapcache_image_blank_color(mapcache_image* image)
{
  if(image->is_blank == MC_EMPTY_UNKNOWN) {
    int* pixptr;
    int r,c;
    for(r=0; r<image->h; r++) {
      pixptr = (int*)(image->data + r * image->stride);
      for(c=0; c<image->w; c++) {
        if(*(pixptr++) != *((int*)image->data)) {
          image->is_blank = MC_EMPTY_NO;
          return MAPCACHE_FALSE;
        }
      }
    }
    image->is_blank = MC_EMPTY_YES;
  }
  assert(image->is_blank != MC_EMPTY_UNKNOWN);
  if(image->is_blank == MC_EMPTY_YES)
    return MAPCACHE_TRUE;
  else
    return MAPCACHE_FALSE;
}

/* vim: ts=2 sts=2 et sw=2
*/
