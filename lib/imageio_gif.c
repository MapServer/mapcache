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

#include "mapcache-config.h"
#ifdef USE_GIF

#include "mapcache.h"
#include <gif_lib.h>
#include <apr_strings.h>

#ifdef _WIN32
typedef unsigned char     uint8_t;
typedef unsigned short    uint16_t;
typedef unsigned int      uint32_t;
typedef unsigned long int uint64_t;
#endif

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

/**\addtogroup imageio_gif */
/** @{ */
typedef struct _mapcache_buffer_closure _mapcache_buffer_closure;
struct _mapcache_buffer_closure{
   mapcache_buffer *buffer;
   mapcache_image *image;
   char * ptr;
   size_t remaining;
};

static int _mapcache_imageio_gif_read_func(GifFileType * gif, GifByteType * buff, int length)
{
   _mapcache_buffer_closure *bc = (_mapcache_buffer_closure*)gif->UserData;
   length = MIN(length, bc->remaining);
   memcpy(buff, bc->ptr, length);
   bc->remaining -= length;
   bc->ptr += length;
   return length;
}

void _mapcache_imageio_gif_decode_to_image(mapcache_context *ctx, mapcache_buffer *buffer,
        mapcache_image *img)
{
static const  unsigned int InterlacedOffset[] = { 0, 4, 2, 1 }; /* The way Interlaced image should. */
static const  unsigned int InterlacedJumps[] = { 8, 8, 4, 2 };    /* be read - offsets and jumps... */

   _mapcache_buffer_closure bc;
   GifFileType * gif;
   GifRecordType record_type;
   int ExtCode;
   GifByteType *Extension, *c, *rgba;
   static GifColorType *ColorMapEntry;
   static ColorMapObject *ColorMap;
   int Row, Col, Width, Height, i,j,n, transparent;

   bc.ptr       = buffer->buf;
   bc.remaining = buffer->size;
   if((gif = DGifOpen(&bc, _mapcache_imageio_gif_read_func)) == NULL)
     goto _error;

   img->w = gif->SWidth;
   img->h = gif->SHeight;
   transparent = -1;

   if(!img->data) {
      img->data = calloc(1, img->w * img->h * 4 * sizeof(unsigned char));
      apr_pool_cleanup_register(ctx->pool, img->data, (void*)free, apr_pool_cleanup_null) ;
      img->stride = img->w * 4;
   }

_read_record:
     if(DGifGetRecordType(gif, &record_type) == GIF_ERROR)
       goto _error;

     if(record_type == IMAGE_DESC_RECORD_TYPE)
       goto _read_desc;
     if(record_type == EXTENSION_RECORD_TYPE)
       goto _read_ext;
     if(record_type == TERMINATE_RECORD_TYPE)
       goto _out;
     goto _read_record;

_read_desc:
     if (DGifGetImageDesc(gif) == GIF_ERROR)
       goto _error;
     {
       Row    = gif->Image.Top; /* Image Position relative to Screen. */
       Col    = gif->Image.Left;
       Width  = gif->Image.Width;
       Height = gif->Image.Height;
       ColorMap = gif->Image.ColorMap? gif->Image.ColorMap: gif->SColorMap;
       if(Col + Width > gif->SWidth || Row + Height > gif->SHeight || !ColorMap)
         goto _error;
       if(gif->Image.Interlace) {
         /* Need to perform 4 passes on the images: */
         for (i = 0; i < 4; i++)
           for (j = Row + InterlacedOffset[i]; j < Row + Height;
                j += InterlacedJumps[i])
           {
             if (DGifGetLine(gif, &(img->data[j * img->stride + Col]), Width) == GIF_ERROR)
               goto _error;
           }
       } else {
         for (i = Row; i < Row + Height; i++) {
           if(DGifGetLine(gif, &img->data[i * img->stride + Col],
                Width) == GIF_ERROR)
             goto _error;
         }
       }
     }
     for (i = Row; i < Row + Height; i++) {
       c    =  &(img->data[i * img->stride + Col + Width -1]);
       rgba =  &(img->data[i * img->stride + img->stride -1]);
       for(n = Width; n > 0; n--, c--) {
         ColorMapEntry = &ColorMap->Colors[*c];
         *rgba-- = (*c == transparent)? 0: 255;
         *rgba-- = ColorMapEntry->Red;
         *rgba-- = ColorMapEntry->Green;
         *rgba-- = ColorMapEntry->Blue;
       }
     }
     goto _out;

_read_ext:
     /* Skip any extension blocks in file: */
     if (DGifGetExtension(gif, &ExtCode, &Extension) == GIF_ERROR)
       goto _error;
     if(ExtCode == 0xf9 && (Extension[1] & 0x01))
       transparent = Extension[4];
     while (Extension != NULL) {
       if (DGifGetExtensionNext(gif, &Extension) == GIF_ERROR)
         goto _error;
     }
     goto _read_record;

_error:
_out:
  DGifCloseFile(gif);
}

mapcache_image* _mapcache_imageio_gif_decode(mapcache_context *ctx, mapcache_buffer *buffer)
{
   mapcache_image *img = mapcache_image_create(ctx);
   _mapcache_imageio_gif_decode_to_image(ctx,buffer,img);
   if(GC_HAS_ERROR(ctx))
      return NULL;
   return img;
}

/** @} */

/* vim: ai ts=3 sts=3 et sw=3
*/
#endif /* USE_GIF */
