/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: GIF format
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
#include <gif_lib.h>


void mapcache_imageio_gif_write_data_to_buffer(GifFileType  * _gif, unsigned char *_buf, int _len)
{
    mapcache_buffer *buffer = _gif->UserData;
    mapcache_buffer_append(buffer, _len, _buf);
}

mapcache_buffer* _mapcache_imageio_gif_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  /* Step 1: Open the file pointer */
  GifFileType *GifFile;
  int i, row;
  ColorMapObject *ColorMap;
  mapcache_buffer *buffer = NULL;
  unsigned int numPaletteEntries = 256;
  rgbaPixel palette[256];
  unsigned int maxval;
  unsigned char *pixels = (unsigned char*)apr_pcalloc(ctx->pool,img->w*img->h*sizeof(unsigned char));
  int numEntries;

  buffer = mapcache_buffer_create(5000,ctx->pool);

  GifFile = EGifOpen(buffer, (OutputFunc)mapcache_imageio_gif_write_data_to_buffer);

  if(GifFile == NULL)
  {
      ctx->set_error(ctx,500,"failed to create GIF image buffer");
      return NULL;
  }

  /* Step 2: Create ColorMap */

  /* Compute the palette, force to 256 */
  if(MAPCACHE_SUCCESS != _mapcache_imageio_quantize_image(img,&numPaletteEntries,palette, &maxval, NULL, 0)) {
      ctx->set_error(ctx,500,"failed to quantize image buffer");
      return NULL;
  }
  if(MAPCACHE_SUCCESS != _mapcache_imageio_classify(img,pixels,palette,numPaletteEntries)) {
      ctx->set_error(ctx,500,"failed to quantize image buffer");
      return NULL;
  }

  /* Assignt the colors to the gif_lib colormap object */
  /* We need to make sure numEntries is a power of two */
  numEntries = 1 << BitSize(numPaletteEntries);
  if ((ColorMap = MakeMapObject(numEntries, NULL)) == NULL)
  {
      ctx->set_error(ctx,500,"failed to create GIF color map");
      return NULL;
  }

  for(i=0; i<numPaletteEntries; i++)
  {
      ColorMap->Colors[i].Red = palette[i].r;
      ColorMap->Colors[i].Green = palette[i].g;
      ColorMap->Colors[i].Blue = palette[i].b;
  }

  /* Step 3: Write the gif header */
  if (EGifPutScreenDesc(GifFile, img->w, img->h, 256, 0, ColorMap) == GIF_ERROR)
  {
      ctx->set_error(ctx,500,"failed to create GIF screen");
      return NULL;
  }

//  if(map->transparent == MS_ON)
  {
      GifByteType extentionblock[4] = {0x01, 0x00, 0x00, 0x00};
      EGifPutExtension(GifFile, 0xf9, 4, &extentionblock);
  }

    /* Dump out the image descriptor: */
  if (EGifPutImageDesc(GifFile, 0, 0, img->w, img->h, 0, NULL) == GIF_ERROR)
  {
      ctx->set_error(ctx,500,"failed to create GIF image desc");
      return NULL;
  }

  /* Step 4: Write the gif content, line by line */
  for(row=0; row<img->h; row++)
  {
      if (EGifPutLine(GifFile, pixels+row*img->w, img->w) == GIF_ERROR)
      {
          ctx->set_error(ctx,500,"failed to put line in GIF");
          return NULL;
      }
  }

    /* Step 5: Close file and write footer */
    if (EGifCloseFile(GifFile) == GIF_ERROR)
    {
        ctx->set_error(ctx,500,"failed to close GIF");
        return NULL;
    }
    return buffer;
}

mapcache_buffer* _mapcache_imageio_animated_gif_encode(mapcache_context *ctx, mapcache_image **images, int numimages, mapcache_image_format * format)
{
  /* Step 1: Open the file pointer */
  mapcache_image *img = images[0];
  GifFileType *GifFile;
  int i, row;
  ColorMapObject *ColorMap;
  mapcache_buffer *buffer = NULL;
  unsigned int numPaletteEntries = 256;
  rgbaPixel palette[256];
  unsigned int maxval;
  unsigned char *pixels = (unsigned char*)apr_pcalloc(ctx->pool,img->w*img->h*sizeof(unsigned char));
  int numEntries;

  buffer = mapcache_buffer_create(5000,ctx->pool);

  GifFile = EGifOpen(buffer, (OutputFunc)mapcache_imageio_gif_write_data_to_buffer);

  if(GifFile == NULL)
  {
      ctx->set_error(ctx,500,"failed to create GIF image buffer");
      return NULL;
  }

  /* Step 2: Create ColorMap */

  /* Compute the palette, force to 256 */
  if(MAPCACHE_SUCCESS != _mapcache_imageio_quantize_image(img,&numPaletteEntries,palette, &maxval, NULL, 0)) {
      ctx->set_error(ctx,500,"failed to quantize image buffer");
      return NULL;
  }

  /* Assignt the colors to the gif_lib colormap object */
  /* We need to make sure numEntries is a power of two */
  numEntries = 1 << BitSize(numPaletteEntries);
  if ((ColorMap = MakeMapObject(numEntries, NULL)) == NULL)
  {
      ctx->set_error(ctx,500,"failed to create GIF color map");
      return NULL;
  }

  for(i=0; i<numPaletteEntries; i++)
  {
      ColorMap->Colors[i].Red = palette[i].r;
      ColorMap->Colors[i].Green = palette[i].g;
      ColorMap->Colors[i].Blue = palette[i].b;
  }

  /* Step 3: Write the gif header */
  if (EGifPutScreenDesc(GifFile, img->w, img->h, 256, 0, ColorMap) == GIF_ERROR)
  {
      ctx->set_error(ctx,500,"failed to create GIF screen");
      return NULL;
  }

  /* first extension block is NETSCAPE2.0*/
  {
    char nsle[12] = "NETSCAPE2.0";
    char subblock[3];
    if (EGifPutExtensionFirst(GifFile, APPLICATION_EXT_FUNC_CODE, 11, nsle) == GIF_ERROR) {
      ctx->set_error(ctx,500,"failed to create GIF extension block");
      return NULL;
    }
    /* The following define the loop count. 0 == infinite */
    subblock[0] = 1;
    subblock[2] = 0;
    subblock[1] = 0;
    if (EGifPutExtensionLast(GifFile, APPLICATION_EXT_FUNC_CODE, 3, subblock) == GIF_ERROR) {
      ctx->set_error(ctx,500,"failed to create GIF extension block");
      return NULL;
    }
  }

  for(i=0; i<numimages; i++) {

    img = images[i];
    if(MAPCACHE_SUCCESS != _mapcache_imageio_classify(img,pixels,palette,numPaletteEntries)) {
      ctx->set_error(ctx,500,"failed to quantize image buffer");
      return NULL;
    }

//  if(map->transparent == MS_ON)
    {
      /* The first byte is a bit mask: 1=transparent, 4=stack frames, 8=erase old frames */
      GifByteType extentionblock[4] = {0x09, 0x64, 0x00, 0x00};
      EGifPutExtension(GifFile, 0xf9, 4, &extentionblock);
    }

    /* Dump out the image descriptor: */
    if (EGifPutImageDesc(GifFile, 0, 0, img->w, img->h, 0, NULL) == GIF_ERROR)
    {
      ctx->set_error(ctx,500,"failed to create GIF image desc");
      return NULL;
    }

  /* Step 4: Write the gif content, line by line */
    for(row=0; row<img->h; row++)
    {
      if (EGifPutLine(GifFile, pixels+row*img->w, img->w) == GIF_ERROR)
      {
          ctx->set_error(ctx,500,"failed to put line in GIF");
          return NULL;
      }
    }
  }

  /* Step 5: Close file and write footer */
  if (EGifCloseFile(GifFile) == GIF_ERROR)
  {
      ctx->set_error(ctx,500,"failed to close GIF");
      return NULL;
  }
  return buffer;
}

mapcache_image_format* mapcache_imageio_create_gif_format(apr_pool_t *pool, char *name)
{
  mapcache_image_format_gif *format = apr_pcalloc(pool, sizeof(mapcache_image_format_gif));
  format->format.name = name;
  format->format.extension = apr_pstrdup(pool,"gif");
  format->format.mime_type = apr_pstrdup(pool,"image/gif");
  format->format.metadata = apr_table_make(pool,3);
  format->format.write = _mapcache_imageio_gif_encode;
  format->format.write_frames = _mapcache_imageio_animated_gif_encode;
  format->format.type = GC_GIF;
  format->animate = NULL;
  return (mapcache_image_format*)format;
}

/** @} */

/* vim: ts=2 sts=2 et sw=2
*/
