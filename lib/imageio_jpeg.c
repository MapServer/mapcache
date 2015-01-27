/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: JPEG format
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
#include <apr_strings.h>
#include <jpeglib.h>

/**\addtogroup imageio_jpg */
/** @{ */

typedef struct {
  struct jpeg_destination_mgr pub;
  unsigned char *data;
  mapcache_buffer *buffer;
} mapcache_jpeg_destination_mgr;


#define OUTPUT_BUF_SIZE 4096

static void _mapcache_imageio_jpeg_init_source(j_decompress_ptr cinfo)
{
  /* nothing to do */
}

static int _mapcache_imageio_jpeg_fill_input_buffer(j_decompress_ptr cinfo)
{
  static JOCTET mybuffer[4];

  /* The whole JPEG data is expected to reside in the supplied memory
   * buffer, so any request for more data beyond the given buffer size
   * is treated as an error.
   */
  /* Insert a fake EOI marker */
  mybuffer[0] = (JOCTET) 0xFF;
  mybuffer[1] = (JOCTET) JPEG_EOI;

  cinfo->src->next_input_byte = mybuffer;
  cinfo->src->bytes_in_buffer = 2;

  return TRUE;
}

static void _mapcache_imageio_jpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
  struct jpeg_source_mgr * src = cinfo->src;

  /* Just a dumb implementation for now.  Could use fseek() except
   * it doesn't work on pipes.  Not clear that being smart is worth
   * any trouble anyway --- large skips are infrequent.
   */
  if (num_bytes > 0) {
    while (num_bytes > (long) src->bytes_in_buffer) {
      num_bytes -= (long) src->bytes_in_buffer;
      (void) (*src->fill_input_buffer) (cinfo);
      /* note we assume that fill_input_buffer will never return FALSE,
       * so suspension need not be handled.
       */
    }
    src->next_input_byte += (size_t) num_bytes;
    src->bytes_in_buffer -= (size_t) num_bytes;
  }
}

static void _mapcache_imageio_jpeg_term_source(j_decompress_ptr cinfo)
{
}



int _mapcache_imageio_jpeg_mem_src (j_decompress_ptr cinfo, unsigned char * inbuffer, unsigned long insize)
{
  struct jpeg_source_mgr * src;

  if (inbuffer == NULL || insize == 0) /* Treat empty input as fatal error */
    return MAPCACHE_FAILURE;

  /* The source object is made permanent so that a series of JPEG images
   * can be read from the same buffer by calling jpeg_mem_src only before
   * the first one.
   */
  if (cinfo->src == NULL) {   /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
                 (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
                     sizeof(struct jpeg_source_mgr));
  }

  src = cinfo->src;
  src->init_source = _mapcache_imageio_jpeg_init_source;
  src->fill_input_buffer = _mapcache_imageio_jpeg_fill_input_buffer;
  src->skip_input_data = _mapcache_imageio_jpeg_skip_input_data;
  src->resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->term_source = _mapcache_imageio_jpeg_term_source;
  src->bytes_in_buffer = (size_t) insize;
  src->next_input_byte = (JOCTET *) inbuffer;
  return MAPCACHE_SUCCESS;
}



void _mapcache_imageio_jpeg_init_destination (j_compress_ptr cinfo)
{
  mapcache_jpeg_destination_mgr *dest = (mapcache_jpeg_destination_mgr*) cinfo->dest;

  /* Allocate the output buffer --- it will be released when done with image */
  dest->data = (unsigned char *)(*cinfo->mem->alloc_small) ((j_common_ptr) cinfo,
               JPOOL_IMAGE,OUTPUT_BUF_SIZE * sizeof (unsigned char));

  dest->pub.next_output_byte = dest->data;
  dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
}

void _mapcache_imageio_jpeg_buffer_term_destination (j_compress_ptr cinfo)
{
  mapcache_jpeg_destination_mgr *dest = (mapcache_jpeg_destination_mgr*) cinfo->dest;
  mapcache_buffer_append(dest->buffer,OUTPUT_BUF_SIZE-dest->pub.free_in_buffer, dest->data);
  dest->pub.next_output_byte = dest->data;
  dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
}


int _mapcache_imageio_jpeg_buffer_empty_output_buffer (j_compress_ptr cinfo)
{
  mapcache_jpeg_destination_mgr *dest = (mapcache_jpeg_destination_mgr*) cinfo->dest;
  mapcache_buffer_append(dest->buffer,OUTPUT_BUF_SIZE, dest->data);
  dest->pub.next_output_byte = dest->data;
  dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
  return TRUE;
}

mapcache_buffer* _mapcache_imageio_jpeg_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  mapcache_jpeg_destination_mgr *dest;
  JSAMPLE *rowdata;
  unsigned int row;
  mapcache_buffer *buffer = mapcache_buffer_create(5000, ctx->pool);
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  cinfo.dest = (struct jpeg_destination_mgr *)(*cinfo.mem->alloc_small) (
                 (j_common_ptr) &cinfo, JPOOL_PERMANENT,
                 sizeof (mapcache_jpeg_destination_mgr));
  ((mapcache_jpeg_destination_mgr*)cinfo.dest)->pub.empty_output_buffer = _mapcache_imageio_jpeg_buffer_empty_output_buffer;
  ((mapcache_jpeg_destination_mgr*)cinfo.dest)->pub.term_destination = _mapcache_imageio_jpeg_buffer_term_destination;
  ((mapcache_jpeg_destination_mgr*)cinfo.dest)->buffer = buffer;

  dest = (mapcache_jpeg_destination_mgr*) cinfo.dest;
  dest->pub.init_destination = _mapcache_imageio_jpeg_init_destination;

  cinfo.image_width = img->w;
  cinfo.image_height = img->h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, ((mapcache_image_format_jpeg*)format)->quality, TRUE);
  switch(((mapcache_image_format_jpeg*)format)->photometric) {
    case MAPCACHE_PHOTOMETRIC_RGB:
      jpeg_set_colorspace(&cinfo, JCS_RGB);
      break;
    case MAPCACHE_PHOTOMETRIC_YCBCR:
    default:
      jpeg_set_colorspace(&cinfo, JCS_YCbCr);
  }
  switch(((mapcache_image_format_jpeg*)format)->optimize) {
    case MAPCACHE_OPTIMIZE_NO:
      cinfo.optimize_coding = FALSE;
      break;
    case MAPCACHE_OPTIMIZE_ARITHMETIC:
      cinfo.optimize_coding = FALSE;
      cinfo.arith_code = TRUE;
      break;
    case MAPCACHE_OPTIMIZE_YES:
    default:
      cinfo.optimize_coding = TRUE;
  }
  jpeg_start_compress(&cinfo, TRUE);

  rowdata = (JSAMPLE*)malloc(img->w*cinfo.input_components*sizeof(JSAMPLE));
  for(row=0; row<img->h; row++) {
    JSAMPLE *pixptr = rowdata;
    int col;
    unsigned char *r,*g,*b;
    r=&(img->data[2])+row*img->stride;
    g=&(img->data[1])+row*img->stride;
    b=&(img->data[0])+row*img->stride;
    for(col=0; col<img->w; col++) {
      *(pixptr++) = *r;
      *(pixptr++) = *g;
      *(pixptr++) = *b;
      r+=4;
      g+=4;
      b+=4;
    }
    (void) jpeg_write_scanlines(&cinfo, &rowdata, 1);
  }

  /* Step 6: Finish compression */

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  free(rowdata);
  return buffer;
}

void _mapcache_imageio_jpeg_decode_to_image(mapcache_context *r, mapcache_buffer *buffer,
    mapcache_image *img)
{
  int s;
  struct jpeg_decompress_struct cinfo = {NULL};
  struct jpeg_error_mgr jerr;
  unsigned char *temp;
  jpeg_create_decompress(&cinfo);
  cinfo.err = jpeg_std_error(&jerr);
  if (_mapcache_imageio_jpeg_mem_src(&cinfo,buffer->buf, buffer->size) != MAPCACHE_SUCCESS) {
    r->set_error(r,500,"failed to allocate jpeg decoding struct");
    return;
  }

  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);
  img->w = cinfo.output_width;
  img->h = cinfo.output_height;
  s = cinfo.output_components;
  if(!img->data) {
    img->data = calloc(1,img->w*img->h*4*sizeof(unsigned char));
    apr_pool_cleanup_register(r->pool, img->data, (void*)free, apr_pool_cleanup_null) ;
    img->stride = img->w * 4;
  }

  temp = malloc(img->w*s);
  apr_pool_cleanup_register(r->pool, temp, (void*)free, apr_pool_cleanup_null) ;
  while ((int)cinfo.output_scanline < img->h) {
    int i;
    unsigned char *rowptr = &img->data[cinfo.output_scanline * img->stride];
    unsigned char *tempptr = temp;
    jpeg_read_scanlines(&cinfo, &tempptr, 1);
    if (s == 1) {
      for (i = 0; i < img->w; i++) {
        *rowptr++ = *tempptr;
        *rowptr++ = *tempptr;
        *rowptr++ = *tempptr;
        *rowptr++ = 255;
        tempptr++;
      }
    } else if (s == 3) {
      for (i = 0; i < img->w; i++) {
        rowptr[0] = tempptr[2];
        rowptr[1] = tempptr[1];
        rowptr[2] = tempptr[0];
        rowptr[3] = 255;
        rowptr+=4;
        tempptr+=3;
      }
    } else {
      r->set_error(r, 500, "unsupported jpeg format");
      jpeg_destroy_decompress(&cinfo);
      return;
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
}

mapcache_image* _mapcache_imageio_jpeg_decode(mapcache_context *r, mapcache_buffer *buffer)
{
  mapcache_image *img = mapcache_image_create(r);
  _mapcache_imageio_jpeg_decode_to_image(r, buffer,img);
  if(GC_HAS_ERROR(r)) {
    return NULL;
  }
  return img;

}

static mapcache_buffer* _mapcache_imageio_jpg_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  mapcache_image *empty;
  mapcache_buffer *buf;
  int i;
  apr_pool_t *pool = NULL;
  if(apr_pool_create(&pool,ctx->pool) != APR_SUCCESS) {
    ctx->set_error(ctx,500,"png create empty: failed to create temp memory pool");
    return NULL;
  }
  empty = mapcache_image_create(ctx);
  if(GC_HAS_ERROR(ctx)) {
    return NULL;
  }
  empty->data = malloc(width*height*4*sizeof(unsigned char));
  for(i=0; i<width*height; i++) {
    ((unsigned int*)empty->data)[i] = color;
  }
  empty->w = width;
  empty->h = height;
  empty->stride = width * 4;

  buf = format->write(ctx,empty,format);
  apr_pool_destroy(pool);
  free(empty->data);
  return buf;
}

mapcache_image_format* mapcache_imageio_create_jpeg_format(apr_pool_t *pool, char *name, int quality,
    mapcache_photometric photometric, mapcache_optimization optimize)
{
  mapcache_image_format_jpeg *format = apr_pcalloc(pool, sizeof(mapcache_image_format_jpeg));
  format->format.name = name;
  format->format.extension = apr_pstrdup(pool,"jpg");
  format->format.mime_type = apr_pstrdup(pool,"image/jpeg");
  format->format.metadata = apr_table_make(pool,3);
  format->format.create_empty_image = _mapcache_imageio_jpg_create_empty;
  format->format.write = _mapcache_imageio_jpeg_encode;
  format->quality = quality;
  format->optimize = optimize;
  format->photometric = photometric;
  format->format.type = GC_JPEG;
  return (mapcache_image_format*)format;
}

/** @} */



/* vim: ts=2 sts=2 et sw=2
*/
