/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: PNG format
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
#include <png.h>
#include <apr_strings.h>

#ifdef _WIN32
typedef unsigned char     uint8_t;
typedef unsigned short    uint16_t;
typedef unsigned int      uint32_t;
typedef unsigned long int uint64_t;
#endif

#ifndef Z_BEST_SPEED
#define Z_BEST_SPEED 1
#endif
#ifndef Z_BEST_COMPRESSION
#define Z_BEST_COMPRESSION 9
#endif
#ifndef Z_NO_COMPRESSION
#define Z_NO_COMPRESSION 0
#endif

/* Table of CRCs of all 8-bit messages. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
unsigned long crc_table[256] = {
  0, 1996959894, 3993919788, 2567524794, 124634137, 1886057615, 3915621685,
  2657392035, 249268274, 2044508324, 3772115230, 2547177864, 162941995,
  2125561021, 3887607047, 2428444049, 498536548, 1789927666, 4089016648,
  2227061214, 450548861, 1843258603, 4107580753, 2211677639, 325883990,
  1684777152, 4251122042, 2321926636, 335633487, 1661365465, 4195302755,
  2366115317, 997073096, 1281953886, 3579855332, 2724688242, 1006888145,
  1258607687, 3524101629, 2768942443, 901097722, 1119000684, 3686517206,
  2898065728, 853044451, 1172266101, 3705015759, 2882616665, 651767980,
  1373503546, 3369554304, 3218104598, 565507253, 1454621731, 3485111705,
  3099436303, 671266974, 1594198024, 3322730930, 2970347812, 795835527,
  1483230225, 3244367275, 3060149565, 1994146192, 31158534, 2563907772,
  4023717930, 1907459465, 112637215, 2680153253, 3904427059, 2013776290,
  251722036, 2517215374, 3775830040, 2137656763, 141376813, 2439277719,
  3865271297, 1802195444, 476864866, 2238001368, 4066508878, 1812370925,
  453092731, 2181625025, 4111451223, 1706088902, 314042704, 2344532202,
  4240017532, 1658658271, 366619977, 2362670323, 4224994405, 1303535960,
  984961486, 2747007092, 3569037538, 1256170817, 1037604311, 2765210733,
  3554079995, 1131014506, 879679996, 2909243462, 3663771856, 1141124467,
  855842277, 2852801631, 3708648649, 1342533948, 654459306, 3188396048,
  3373015174, 1466479909, 544179635, 3110523913, 3462522015, 1591671054,
  702138776, 2966460450, 3352799412, 1504918807, 783551873, 3082640443,
  3233442989, 3988292384, 2596254646, 62317068, 1957810842, 3939845945,
  2647816111, 81470997, 1943803523, 3814918930, 2489596804, 225274430,
  2053790376, 3826175755, 2466906013, 167816743, 2097651377, 4027552580,
  2265490386, 503444072, 1762050814, 4150417245, 2154129355, 426522225,
  1852507879, 4275313526, 2312317920, 282753626, 1742555852, 4189708143,
  2394877945, 397917763, 1622183637, 3604390888, 2714866558, 953729732,
  1340076626, 3518719985, 2797360999, 1068828381, 1219638859, 3624741850,
  2936675148, 906185462, 1090812512, 3747672003, 2825379669, 829329135,
  1181335161, 3412177804, 3160834842, 628085408, 1382605366, 3423369109,
  3138078467, 570562233, 1426400815, 3317316542, 2998733608, 733239954,
  1555261956, 3268935591, 3050360625, 752459403, 1541320221, 2607071920,
  3965973030, 1969922972, 40735498, 2617837225, 3943577151, 1913087877,
  83908371, 2512341634, 3803740692, 2075208622, 213261112, 2463272603,
  3855990285, 2094854071, 198958881, 2262029012, 4057260610, 1759359992,
  534414190, 2176718541, 4139329115, 1873836001, 414664567, 2282248934,
  4279200368, 1711684554, 285281116, 2405801727, 4167216745, 1634467795,
  376229701, 2685067896, 3608007406, 1308918612, 956543938, 2808555105,
  3495958263, 1231636301, 1047427035, 2932959818, 3654703836, 1088359270,
  936918000, 2847714899, 3736837829, 1202900863, 817233897, 3183342108,
  3401237130, 1404277552, 615818150, 3134207493, 3453421203, 1423857449,
  601450431, 3009837614, 3294710456, 1567103746, 711928724, 3020668471,
  3272380065, 1510334235, 755167117
};
#else
/* Flag: has the table been computed? Initially false. */
int crc_table_computed = 0;
unsigned long crc_table[256];

/* Make the table for a fast CRC. */
static void make_crc_table(void)
{
  unsigned long c;
  int n, k;

  for (n = 0; n < 256; n++) {
    c = (unsigned long) n;
    for (k = 0; k < 8; k++) {
      if (c & 1)
        c = 0xedb88320L ^ (c >> 1);
      else
        c = c >> 1;
    }
    crc_table[n] = c;
  }
  crc_table_computed = 1;
}
#endif


/* Update a running CRC with the bytes buf[0..len-1]--the CRC
   should be initialized to all 1's, and the transmitted value
   is the 1's complement of the final running CRC (see the
   crc() routine below)). */

static unsigned long update_crc(unsigned long crc, unsigned char *buf,
                                int len)
{
  unsigned long c = crc;
  int n;

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  if (!crc_table_computed)
    make_crc_table();
#endif
  for (n = 0; n < len; n++) {
    c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
  }
  return c;
}

/* Return the CRC of the bytes buf[0..len-1]. */
static unsigned long crc(unsigned char *buf, int len)
{
  return update_crc(0xffffffffL, buf, len) ^ 0xffffffffL;
}

static unsigned char empty_png[] = {
  0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52
  ,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x01,0x03,0x00,0x00,0x00,0x66,0xbc,0x3a
  ,0x25,0x00,0x00,0x00,0x03,0x50,0x4c,0x54,0x45,0x73,0x91,0xad,0x31,0xf0,0x8f,0xdd
  ,0x00,0x00,0x00,0x01,0x74,0x52,0x4e,0x53,0xff,0x6d,0xe4,0x37,0xeb,0x00,0x00,0x00
  ,0x1f,0x49,0x44,0x41,0x54,0x68,0xde,0xed,0xc1,0x01,0x0d,0x00,0x00,0x00,0xc2,0xa0
  ,0xf7,0x4f,0x6d,0x0e,0x37,0xa0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xbe,0x0d
  ,0x21,0x00,0x00,0x01,0x7f,0x19,0x9c,0xa7,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44
  ,0xae,0x42,0x60,0x82
};

static size_t plte_offset = 0x25;
static size_t trns_offset = 0x34;

mapcache_buffer* mapcache_empty_png_decode(mapcache_context *ctx, const unsigned char *hex_color, int *is_empty) {
  int chunkcrc;
  unsigned char *dd;
  mapcache_buffer *encoded_data = mapcache_buffer_create(sizeof(empty_png)+4,ctx->pool);
  dd = encoded_data->buf;
  memcpy(dd,empty_png,sizeof(empty_png));
  dd[plte_offset+4] = hex_color[3]; //r
  dd[plte_offset+5] = hex_color[2]; //g
  dd[plte_offset+6] = hex_color[1]; //b
  chunkcrc = crc(dd+plte_offset,7);
  dd[plte_offset+7] = (unsigned char)((chunkcrc >> 24) & 0xff);
  dd[plte_offset+8] = (unsigned char)((chunkcrc >> 16) & 0xff);
  dd[plte_offset+9] = (unsigned char)((chunkcrc >> 8) & 0xff);
  dd[plte_offset+10] = (unsigned char)(chunkcrc & 0xff);
  if(hex_color[4] != 255) {
    dd[trns_offset+4] = hex_color[4];
    chunkcrc = crc(dd+trns_offset,5);
    dd[trns_offset+5] = (unsigned char)((chunkcrc >> 24) & 0xff);
    dd[trns_offset+6] = (unsigned char)((chunkcrc >> 16) & 0xff);
    dd[trns_offset+7] = (unsigned char)((chunkcrc >> 8) & 0xff);
    dd[trns_offset+8] = (unsigned char)(chunkcrc & 0xff);
  }
  if(hex_color[4] == 0) {
    *is_empty = 1;
  } else {
    *is_empty = 0;
  }
  encoded_data->size = sizeof(empty_png);
  return encoded_data;
}


/**\addtogroup imageio_png */
/** @{ */
typedef struct _mapcache_buffer_closure _mapcache_buffer_closure;
struct _mapcache_buffer_closure {
  mapcache_buffer *buffer;
  unsigned char *ptr;
};

void _mapcache_imageio_png_write_func(png_structp png_ptr, png_bytep data, png_size_t length)
{
  mapcache_buffer_append((mapcache_buffer*)png_get_io_ptr(png_ptr),length,data);
}

void _mapcache_imageio_png_read_func(png_structp png_ptr, png_bytep data, png_size_t length)
{
  _mapcache_buffer_closure *b = (_mapcache_buffer_closure*)png_get_io_ptr(png_ptr);
  memcpy(data,b->ptr,length);
  b->ptr += length;
}

void _mapcache_imageio_png_flush_func(png_structp png_ptr)
{
  // do nothing
}

#ifndef _WIN32
static inline int premultiply (int color,int alpha)
#else
static __inline int premultiply (int color,int alpha)
#endif
{
  int temp = (alpha * color) + 0x80;
  return ((temp + (temp >> 8)) >> 8);
}


void _mapcache_imageio_png_decode_to_image(mapcache_context *ctx, mapcache_buffer *buffer,
    mapcache_image *img)
{
  unsigned char *rowptr;
  png_uint_32 width, height;
  int bit_depth,color_type,i;
  unsigned char **row_pointers;
  png_structp png_ptr = NULL;
  png_infop info_ptr = NULL;
  _mapcache_buffer_closure b;
  b.buffer = buffer;
  b.ptr = buffer->buf;



  /* could pass pointers to user-defined error handlers instead of NULLs: */
  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr) {
    ctx->set_error(ctx, 500, "failed to allocate png_struct structure");
    return;
  }

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    ctx->set_error(ctx, 500, "failed to allocate png_info structure");
    return;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    ctx->set_error(ctx, 500, "failed to setjmp(png_jmpbuf(png_ptr))");
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return;
  }
  png_set_read_fn(png_ptr,&b,_mapcache_imageio_png_read_func);

  png_read_info(png_ptr,info_ptr);
  if(!png_get_IHDR(png_ptr, info_ptr, &width, &height,&bit_depth, &color_type,NULL,NULL,NULL)) {
    ctx->set_error(ctx, 500, "failed to read png header");
    return;
  }

  img->w = width;
  img->h = height;
  if(!img->data) {
    img->data = calloc(1,img->w*img->h*4*sizeof(unsigned char));
    apr_pool_cleanup_register(ctx->pool, img->data, (void*)free, apr_pool_cleanup_null) ;
    img->stride = img->w * 4;
  }
  row_pointers = malloc(img->h * sizeof(unsigned char*));
  apr_pool_cleanup_register(ctx->pool, row_pointers, (void*)free, apr_pool_cleanup_null) ;

  rowptr = img->data;
  for(i=0; i<img->h; i++) {
    row_pointers[i] = rowptr;
    rowptr += img->stride;
  }


  png_set_expand(png_ptr);
  png_set_strip_16(png_ptr);
  png_set_gray_to_rgb(png_ptr);
  png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);

  png_read_update_info(png_ptr, info_ptr);

  png_read_image(png_ptr, row_pointers);

  png_read_end(png_ptr,NULL);
  png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

  /* switch buffer from rgba to premultiplied argb */
  for(i=0; i<img->h; i++) {
    unsigned int j;
    unsigned char pixel[4];
    uint8_t  alpha;
    unsigned char *pixptr = row_pointers[i];
    for(j=0; j<img->w; j++) {

      memcpy (pixel, pixptr, sizeof (uint32_t));
      alpha = pixel[3];
      if(alpha == 255) {
        pixptr[0] = pixel[2];
        pixptr[1] = pixel[1];
        pixptr[2] = pixel[0];
      } else if (alpha == 0) {
        pixptr[0] = 0;
        pixptr[1] = 0;
        pixptr[2] = 0;
      } else {
        pixptr[0] = premultiply(pixel[2],alpha);
        pixptr[1] = premultiply(pixel[1],alpha);
        pixptr[2] = premultiply(pixel[0],alpha);
      }
      pixptr += 4;
    }
  }

}


mapcache_image* _mapcache_imageio_png_decode(mapcache_context *ctx, mapcache_buffer *buffer)
{
  mapcache_image *img = mapcache_image_create(ctx);
  _mapcache_imageio_png_decode_to_image(ctx,buffer,img);
  if(GC_HAS_ERROR(ctx))
    return NULL;
  return img;
}

/* png transform function to switch from premultiplied argb to png expected rgba */
static void
argb_to_rgba (png_structp png, png_row_infop row_info, png_bytep data)
{
  unsigned int i;

  for (i = 0; i < row_info->rowbytes; i += 4) {
    uint8_t *b = &data[i];
    uint32_t pixel;
    uint8_t  alpha;

    memcpy (&pixel, b, sizeof (uint32_t));
    alpha = (pixel & 0xff000000) >> 24;
    if (alpha == 0) {
      b[0] = b[1] = b[2] = b[3] = 0;
    } else if (alpha == 255) {
      b[0] = (pixel & 0xff0000) >> 16;
      b[1] = (pixel & 0x00ff00) >>  8;
      b[2] = (pixel & 0x0000ff) >>  0;
      b[3] = 255;
    } else {
      b[0] = (((pixel & 0xff0000) >> 16) * 255 + alpha / 2) / alpha;
      b[1] = (((pixel & 0x00ff00) >>  8) * 255 + alpha / 2) / alpha;
      b[2] = (((pixel & 0x0000ff) >>  0) * 255 + alpha / 2) / alpha;
      b[3] = alpha;
    }
  }
}


/* png transform function to switch from xrgb to rgbx (x is ignored)*/
static void
xrgb_to_rgbx (png_structp png, png_row_infop row_info, png_bytep data)
{
  unsigned int i;

  for (i = 0; i < row_info->rowbytes; i += 4) {
    uint8_t *b = &data[i];
    uint32_t pixel;

    memcpy (&pixel, b, sizeof (uint32_t));

    b[0] = (pixel & 0xff0000) >> 16;
    b[1] = (pixel & 0x00ff00) >>  8;
    b[2] = (pixel & 0x0000ff) >>  0;
  }
}



/**
 * \brief encode an image to RGB(A) PNG format
 * \private \memberof mapcache_image_format_png
 * \sa mapcache_image_format::write()
 */
mapcache_buffer* _mapcache_imageio_png_encode(mapcache_context *ctx, mapcache_image *img, mapcache_image_format *format)
{
  png_bytep rowptr;
  png_infop info_ptr;
  int color_type;
  size_t row;
  mapcache_buffer *buffer = NULL;
  int compression = ((mapcache_image_format_png*)format)->compression_level;
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,NULL,NULL);
  if (!png_ptr) {
    ctx->set_error(ctx, 500, "failed to allocate png_struct structure");
    return NULL;
  }
  if(compression == MAPCACHE_COMPRESSION_BEST)
    png_set_compression_level (png_ptr, Z_BEST_COMPRESSION);
  else if(compression == MAPCACHE_COMPRESSION_FAST)
    png_set_compression_level (png_ptr, Z_BEST_SPEED);
  else if(compression == MAPCACHE_COMPRESSION_DISABLE)
    png_set_compression_level (png_ptr, Z_NO_COMPRESSION);
    
  png_set_filter(png_ptr,0,PNG_FILTER_NONE);

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr,
                             (png_infopp)NULL);
    ctx->set_error(ctx, 500, "failed to allocate png_info structure");
    return NULL;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    ctx->set_error(ctx, 500, "failed to setjmp(png_jmpbuf(png_ptr))");
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return NULL;
  }

  buffer = mapcache_buffer_create(5000,ctx->pool);

  png_set_write_fn(png_ptr, buffer, _mapcache_imageio_png_write_func, _mapcache_imageio_png_flush_func);

  if(mapcache_image_has_alpha(img))
    color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  else
    color_type = PNG_COLOR_TYPE_RGB;

  png_set_IHDR(png_ptr, info_ptr, img->w, img->h,
               8, color_type, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png_ptr, info_ptr);
  if(color_type == PNG_COLOR_TYPE_RGB) {
    png_set_write_user_transform_fn (png_ptr, xrgb_to_rgbx);
    png_set_filler(png_ptr, 255, PNG_FILLER_AFTER);
  } else {
    png_set_write_user_transform_fn (png_ptr, argb_to_rgba);
  }

  rowptr = img->data;
  for(row=0; row<img->h; row++) {
    png_write_row(png_ptr,rowptr);
    rowptr += img->stride;
  }
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  return buffer;
}

/** \cond DONOTDOCUMENT */

/*
 * derivations from pngquant and ppmquant
 *
 ** pngquant.c - quantize the colors in an alphamap down to a specified number
 **
 ** Copyright (C) 1989, 1991 by Jef Poskanzer.
 ** Copyright (C) 1997, 2000, 2002 by Greg Roelofs; based on an idea by
 **                                Stefan Schneider.
 **
 ** Permission to use, copy, modify, and distribute this software and its
 ** documentation for any purpose and without fee is hereby granted, provided
 ** that the above copyright notice appear in all copies and that both that
 ** copyright notice and this permission notice appear in supporting
 ** documentation.  This software is provided "as is" without express or
 ** implied warranty.
 */

typedef struct {
  unsigned char b,g,r,a;
} rgbaPixel;

typedef struct {
  unsigned char r,g,b;
} rgbPixel;

#define PAM_GETR(p) ((p).r)
#define PAM_GETG(p) ((p).g)
#define PAM_GETB(p) ((p).b)
#define PAM_GETA(p) ((p).a)
#define PAM_ASSIGN(p,red,grn,blu,alf) \
      do { (p).r = (red); (p).g = (grn); (p).b = (blu); (p).a = (alf); } while (0)
#define PAM_EQUAL(p,q) \
      ((p).r == (q).r && (p).g == (q).g && (p).b == (q).b && (p).a == (q).a)
#define PAM_DEPTH(newp,p,oldmaxval,newmaxval) \
      PAM_ASSIGN( (newp), \
            ( (int) PAM_GETR(p) * (newmaxval) + (oldmaxval) / 2 ) / (oldmaxval), \
            ( (int) PAM_GETG(p) * (newmaxval) + (oldmaxval) / 2 ) / (oldmaxval), \
            ( (int) PAM_GETB(p) * (newmaxval) + (oldmaxval) / 2 ) / (oldmaxval), \
            ( (int) PAM_GETA(p) * (newmaxval) + (oldmaxval) / 2 ) / (oldmaxval) )


/* from pamcmap.h */

typedef struct acolorhist_item *acolorhist_vector;
struct acolorhist_item {
  rgbaPixel acolor;
  int value;
};

typedef struct acolorhist_list_item *acolorhist_list;
struct acolorhist_list_item {
  struct acolorhist_item ch;
  acolorhist_list next;
};

typedef acolorhist_list *acolorhash_table;

#define MAXCOLORS  32767

#define LARGE_NORM
#define REP_AVERAGE_PIXELS

typedef struct box *box_vector;
struct box {
  int ind;
  int colors;
  int sum;
};

static acolorhist_vector mediancut(acolorhist_vector achv, int colors, int sum, unsigned char maxval, int newcolors);
static int redcompare (const void *ch1, const void *ch2);
static int greencompare (const void *ch1, const void *ch2);
static int bluecompare (const void *ch1, const void *ch2);
static int alphacompare (const void *ch1, const void *ch2);
static int sumcompare (const void *b1, const void *b2);

static acolorhist_vector pam_acolorhashtoacolorhist
(acolorhash_table acht, int maxacolors);
static acolorhist_vector pam_computeacolorhist
(rgbaPixel **apixels, int cols, int rows, int maxacolors, int* acolorsP);
static acolorhash_table pam_computeacolorhash
(rgbaPixel** apixels, int cols, int rows, int maxacolors, int* acolorsP);
static acolorhash_table pam_allocacolorhash (void);
static int pam_addtoacolorhash
(acolorhash_table acht, rgbaPixel *acolorP, int value);
static int pam_lookupacolor (acolorhash_table acht, rgbaPixel* acolorP);
static void pam_freeacolorhist (acolorhist_vector achv);
static void pam_freeacolorhash (acolorhash_table acht);


/**
 * Compute a palette for the given RGBA rasterBuffer using a median cut quantization.
 * - rb: the rasterBuffer to quantize
 * - reqcolors: the desired number of colors the palette should contain. will be set
 *   with the actual number of entries in the computed palette
 * - palette: preallocated array of palette entries that will be populated by the
 *   function
 * - maxval: max value of pixel intensity. In some cases, the input data has to
 *   be rescaled to compute the quantization. if the returned value of maxscale is
 *   less than 255, this means that the input pixels have been rescaled, and that
 *   the returned palette must be upscaled before being written to the png file
 * - forced_palette: entries that should appear in the computed palette
 * - num_forced_palette_entries: number of entries contained in "force_palette". if 0,
 *   "force_palette" can be NULL
 */
int _mapcache_imageio_quantize_image(mapcache_image *rb,
                                     unsigned int *reqcolors, rgbaPixel *palette,
                                     unsigned int *maxval,
                                     rgbaPixel *forced_palette, int num_forced_palette_entries)
{

  rgbaPixel **apixels=NULL; /* pointer to the start rows of truecolor pixels */
  register rgbaPixel *pP;
  register int col;

  unsigned char newmaxval;
  acolorhist_vector achv, acolormap=NULL;

  int row;
  int colors;
  int newcolors = 0;

  int x;
  /*  int channels;  */


  *maxval = 255;

  apixels=(rgbaPixel**)malloc(rb->h*sizeof(rgbaPixel**));
  if(!apixels) return MAPCACHE_FAILURE;

  for(row=0; row<rb->h; row++) {
    apixels[row]=(rgbaPixel*)(&(rb->data[row * rb->stride]));
  }

  /*
   ** Step 2: attempt to make a histogram of the colors, unclustered.
   ** If at first we don't succeed, lower maxval to increase color
   ** coherence and try again.  This will eventually terminate, with
   ** maxval at worst 15, since 32^3 is approximately MAXCOLORS.
                 [GRR POSSIBLE BUG:  what about 32^4 ?]
   */
  for ( ; ; ) {
    achv = pam_computeacolorhist(
             apixels, rb->w, rb->h, MAXCOLORS, &colors );
    if ( achv != (acolorhist_vector) 0 )
      break;
    newmaxval = *maxval / 2;
    for ( row = 0; row < rb->h; ++row )
      for ( col = 0, pP = apixels[row]; col < rb->w; ++col, ++pP )
        PAM_DEPTH( *pP, *pP, *maxval, newmaxval );
    *maxval = newmaxval;
  }
  newcolors = MAPCACHE_MIN(colors, *reqcolors);
  acolormap = mediancut(achv, colors, rb->w*rb->h, *maxval, newcolors);
  pam_freeacolorhist(achv);


  *reqcolors = newcolors;


  for (x = 0; x < newcolors; ++x) {
    palette[x].r = acolormap[x].acolor.r;
    palette[x].g = acolormap[x].acolor.g;
    palette[x].b = acolormap[x].acolor.b;
    palette[x].a = acolormap[x].acolor.a;
  }

  free(acolormap);
  free(apixels);
  return MAPCACHE_SUCCESS;
}


int _mapcache_imageio_classify(mapcache_image *rb, unsigned char *pixels,
                               rgbaPixel *palette, int numPaletteEntries)
{
  register int ind;
  unsigned char *outrow,*pQ;
  register rgbaPixel *pP;
  acolorhash_table acht;
  int usehash, row, col;
  /*
   ** Step 4: map the colors in the image to their closest match in the
   ** new colormap, and write 'em out.
   */
  acht = pam_allocacolorhash( );
  usehash = 1;

  for ( row = 0; row < rb->h; ++row ) {
    outrow = &(pixels[row*rb->w]);
    col = 0;
    pP = (rgbaPixel*)(&(rb->data[row * rb->stride]));;
    pQ = outrow;
    do {
      /* Check hash table to see if we have already matched this color. */
      ind = pam_lookupacolor( acht, pP );
      if ( ind == -1 ) {
        /* No; search acolormap for closest match. */
        register int i, r1, g1, b1, a1, r2, g2, b2, a2;
        register long dist, newdist;

        r1 = PAM_GETR( *pP );
        g1 = PAM_GETG( *pP );
        b1 = PAM_GETB( *pP );
        a1 = PAM_GETA( *pP );
        dist = 2000000000;
        for ( i = 0; i < numPaletteEntries; ++i ) {
          r2 = PAM_GETR( palette[i] );
          g2 = PAM_GETG( palette[i] );
          b2 = PAM_GETB( palette[i] );
          a2 = PAM_GETA( palette[i] );
          /* GRR POSSIBLE BUG */
          newdist = ( r1 - r2 ) * ( r1 - r2 ) +  /* may overflow? */
                    ( g1 - g2 ) * ( g1 - g2 ) +
                    ( b1 - b2 ) * ( b1 - b2 ) +
                    ( a1 - a2 ) * ( a1 - a2 );
          if ( newdist < dist ) {
            ind = i;
            dist = newdist;
          }
        }
        if ( usehash ) {
          if ( pam_addtoacolorhash( acht, pP, ind ) < 0 ) {
            usehash = 0;
          }
        }
      }

      /*          *pP = acolormap[ind].acolor;  */
      *pQ = (unsigned char)ind;

      ++col;
      ++pP;
      ++pQ;

    } while ( col != rb->w );
  }
  pam_freeacolorhash(acht);

  return MAPCACHE_SUCCESS;
}



/*
 ** Here is the fun part, the median-cut colormap generator.  This is based
 ** on Paul Heckbert's paper, "Color Image Quantization for Frame Buffer
 ** Display," SIGGRAPH 1982 Proceedings, page 297.
 */

static acolorhist_vector
mediancut(acolorhist_vector achv, int colors, int sum, unsigned char maxval, int newcolors )
{
  acolorhist_vector acolormap;
  box_vector bv;
  register int bi, i;
  int boxes;

  bv = (box_vector) malloc( sizeof(struct box) * newcolors );
  acolormap =
    (acolorhist_vector) malloc( sizeof(struct acolorhist_item) * newcolors);
  if ( bv == (box_vector) 0 || acolormap == (acolorhist_vector) 0 ) {
    fprintf( stderr, "  out of memory allocating box vector\n" );
    fflush(stderr);
    exit(6);
  }
  for ( i = 0; i < newcolors; ++i )
    PAM_ASSIGN( acolormap[i].acolor, 0, 0, 0, 0 );

  /*
   ** Set up the initial box.
   */
  bv[0].ind = 0;
  bv[0].colors = colors;
  bv[0].sum = sum;
  boxes = 1;

  /*
   ** Main loop: split boxes until we have enough.
   */
  while ( boxes < newcolors ) {
    register int indx, clrs;
    int sm;
    register int minr, maxr, ming, mina, maxg, minb, maxb, maxa, v;
    int halfsum, lowersum;

    /*
     ** Find the first splittable box.
     */
    for ( bi = 0; bi < boxes; ++bi )
      if ( bv[bi].colors >= 2 )
        break;
    if ( bi == boxes )
      break;        /* ran out of colors! */
    indx = bv[bi].ind;
    clrs = bv[bi].colors;
    sm = bv[bi].sum;

    /*
     ** Go through the box finding the minimum and maximum of each
     ** component - the boundaries of the box.
     */
    minr = maxr = PAM_GETR( achv[indx].acolor );
    ming = maxg = PAM_GETG( achv[indx].acolor );
    minb = maxb = PAM_GETB( achv[indx].acolor );
    mina = maxa = PAM_GETA( achv[indx].acolor );
    for ( i = 1; i < clrs; ++i ) {
      v = PAM_GETR( achv[indx + i].acolor );
      if ( v < minr ) minr = v;
      if ( v > maxr ) maxr = v;
      v = PAM_GETG( achv[indx + i].acolor );
      if ( v < ming ) ming = v;
      if ( v > maxg ) maxg = v;
      v = PAM_GETB( achv[indx + i].acolor );
      if ( v < minb ) minb = v;
      if ( v > maxb ) maxb = v;
      v = PAM_GETA( achv[indx + i].acolor );
      if ( v < mina ) mina = v;
      if ( v > maxa ) maxa = v;
    }

    /*
     ** Find the largest dimension, and sort by that component.  I have
     ** included two methods for determining the "largest" dimension;
     ** first by simply comparing the range in RGB space, and second
     ** by transforming into luminosities before the comparison.  You
     ** can switch which method is used by switching the commenting on
     ** the LARGE_ defines at the beginning of this source file.
     */
#ifdef LARGE_NORM
    if ( maxa - mina >= maxr - minr && maxa - mina >= maxg - ming && maxa - mina >= maxb - minb )
      qsort(
        (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
        alphacompare );
    else if ( maxr - minr >= maxg - ming && maxr - minr >= maxb - minb )
      qsort(
        (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
        redcompare );
    else if ( maxg - ming >= maxb - minb )
      qsort(
        (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
        greencompare );
    else
      qsort(
        (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
        bluecompare );
#endif /*LARGE_NORM*/
#ifdef LARGE_LUM
    {
      apixel p;
      float rl, gl, bl, al;

      PAM_ASSIGN(p, maxr - minr, 0, 0, 0);
      rl = PPM_LUMIN(p);
      PAM_ASSIGN(p, 0, maxg - ming, 0, 0);
      gl = PPM_LUMIN(p);
      PAM_ASSIGN(p, 0, 0, maxb - minb, 0);
      bl = PPM_LUMIN(p);

      /*
      GRR: treat alpha as grayscale and assign (maxa - mina) to each of R, G, B?
      assign (maxa - mina)/3 to each?
      use alpha-fractional luminosity?  (normalized_alpha * lum(r,g,b))
      al = dunno ...
      [probably should read Heckbert's paper to decide]
       */

      if ( al >= rl && al >= gl && al >= bl )
        qsort(
          (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
          alphacompare );
      else if ( rl >= gl && rl >= bl )
        qsort(
          (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
          redcompare );
      else if ( gl >= bl )
        qsort(
          (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
          greencompare );
      else
        qsort(
          (char*) &(achv[indx]), clrs, sizeof(struct acolorhist_item),
          bluecompare );
    }
#endif /*LARGE_LUM*/

    /*
     ** Now find the median based on the counts, so that about half the
     ** pixels (not colors, pixels) are in each subdivision.
     */
    lowersum = achv[indx].value;
    halfsum = sm / 2;
    for ( i = 1; i < clrs - 1; ++i ) {
      if ( lowersum >= halfsum )
        break;
      lowersum += achv[indx + i].value;
    }

    /*
     ** Split the box, and sort to bring the biggest boxes to the top.
     */
    bv[bi].colors = i;
    bv[bi].sum = lowersum;
    bv[boxes].ind = indx + i;
    bv[boxes].colors = clrs - i;
    bv[boxes].sum = sm - lowersum;
    ++boxes;
    qsort( (char*) bv, boxes, sizeof(struct box), sumcompare );
  }

  /*
   ** Ok, we've got enough boxes.  Now choose a representative color for
   ** each box.  There are a number of possible ways to make this choice.
   ** One would be to choose the center of the box; this ignores any structure
   ** within the boxes.  Another method would be to average all the colors in
   ** the box - this is the method specified in Heckbert's paper.  A third
   ** method is to average all the pixels in the box.  You can switch which
   ** method is used by switching the commenting on the REP_ defines at
   ** the beginning of this source file.
   */
  for ( bi = 0; bi < boxes; ++bi ) {
#ifdef REP_CENTER_BOX
    register int indx = bv[bi].ind;
    register int clrs = bv[bi].colors;
    register int minr, maxr, ming, maxg, minb, maxb, mina, maxa, v;

    minr = maxr = PAM_GETR( achv[indx].acolor );
    ming = maxg = PAM_GETG( achv[indx].acolor );
    minb = maxb = PAM_GETB( achv[indx].acolor );
    mina = maxa = PAM_GETA( achv[indx].acolor );
    for ( i = 1; i < clrs; ++i ) {
      v = PAM_GETR( achv[indx + i].acolor );
      minr = min( minr, v );
      maxr = max( maxr, v );
      v = PAM_GETG( achv[indx + i].acolor );
      ming = min( ming, v );
      maxg = max( maxg, v );
      v = PAM_GETB( achv[indx + i].acolor );
      minb = min( minb, v );
      maxb = max( maxb, v );
      v = PAM_GETA( achv[indx + i].acolor );
      mina = min( mina, v );
      maxa = max( maxa, v );
    }
    PAM_ASSIGN(
      acolormap[bi].acolor, ( minr + maxr ) / 2, ( ming + maxg ) / 2,
      ( minb + maxb ) / 2, ( mina + maxa ) / 2 );
#endif /*REP_CENTER_BOX*/
#ifdef REP_AVERAGE_COLORS
    register int indx = bv[bi].ind;
    register int clrs = bv[bi].colors;
    register long r = 0, g = 0, b = 0, a = 0;

    for ( i = 0; i < clrs; ++i ) {
      r += PAM_GETR( achv[indx + i].acolor );
      g += PAM_GETG( achv[indx + i].acolor );
      b += PAM_GETB( achv[indx + i].acolor );
      a += PAM_GETA( achv[indx + i].acolor );
    }
    r = r / clrs;
    g = g / clrs;
    b = b / clrs;
    a = a / clrs;
    PAM_ASSIGN( acolormap[bi].acolor, r, g, b, a );
#endif /*REP_AVERAGE_COLORS*/
#ifdef REP_AVERAGE_PIXELS
    register int indx = bv[bi].ind;
    register int clrs = bv[bi].colors;
    register long r = 0, g = 0, b = 0, a = 0, sum = 0;

    for ( i = 0; i < clrs; ++i ) {
      r += PAM_GETR( achv[indx + i].acolor ) * achv[indx + i].value;
      g += PAM_GETG( achv[indx + i].acolor ) * achv[indx + i].value;
      b += PAM_GETB( achv[indx + i].acolor ) * achv[indx + i].value;
      a += PAM_GETA( achv[indx + i].acolor ) * achv[indx + i].value;
      sum += achv[indx + i].value;
    }
    r = r / sum;
    if ( r > maxval ) r = maxval;        /* avoid math errors */
    g = g / sum;
    if ( g > maxval ) g = maxval;
    b = b / sum;
    if ( b > maxval ) b = maxval;
    a = a / sum;
    if ( a > maxval ) a = maxval;
    /* GRR 20001228:  added casts to quiet warnings; 255 DEPENDENCY */
    PAM_ASSIGN( acolormap[bi].acolor, (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a );
#endif /*REP_AVERAGE_PIXELS*/
  }

  /*
   ** All done.
   */
  free(bv);
  return acolormap;
}

static int
redcompare( const void *ch1, const void *ch2 )
{
  return (int) PAM_GETR( ((acolorhist_vector)ch1)->acolor ) -
         (int) PAM_GETR( ((acolorhist_vector)ch2)->acolor );
}

static int
greencompare( const void *ch1, const void *ch2 )
{
  return (int) PAM_GETG( ((acolorhist_vector)ch1)->acolor ) -
         (int) PAM_GETG( ((acolorhist_vector)ch2)->acolor );
}

static int
bluecompare( const void *ch1, const void *ch2 )
{
  return (int) PAM_GETB( ((acolorhist_vector)ch1)->acolor ) -
         (int) PAM_GETB( ((acolorhist_vector)ch2)->acolor );
}

static int
alphacompare( const void *ch1, const void *ch2 )
{
  return (int) PAM_GETA( ((acolorhist_vector)ch1)->acolor ) -
         (int) PAM_GETA( ((acolorhist_vector)ch2)->acolor );
}

static int
sumcompare( const void *b1, const void *b2 )
{
  return ((box_vector)b2)->sum -
         ((box_vector)b1)->sum;
}


/*===========================================================================*/


/* libpam3.c - pam (portable alpha map) utility library part 3
 **
 ** Colormap routines.
 **
 ** Copyright (C) 1989, 1991 by Jef Poskanzer.
 ** Copyright (C) 1997 by Greg Roelofs.
 **
 ** Permission to use, copy, modify, and distribute this software and its
 ** documentation for any purpose and without fee is hereby granted, provided
 ** that the above copyright notice appear in all copies and that both that
 ** copyright notice and this permission notice appear in supporting
 ** documentation.  This software is provided "as is" without express or
 ** implied warranty.
 */

/*
#include "pam.h"
#include "pamcmap.h"
 */

#define HASH_SIZE 20023

#define pam_hashapixel(p) ( ( ( (long) PAM_GETR(p) * 33023 + \
      (long) PAM_GETG(p) * 30013 + \
      (long) PAM_GETB(p) * 27011 + \
      (long) PAM_GETA(p) * 24007 ) \
      & 0x7fffffff ) % HASH_SIZE )

static acolorhist_vector
pam_computeacolorhist( apixels, cols, rows, maxacolors, acolorsP )
rgbaPixel** apixels;
int cols, rows, maxacolors;
int* acolorsP;
{
  acolorhash_table acht;
  acolorhist_vector achv;

  acht = pam_computeacolorhash( apixels, cols, rows, maxacolors, acolorsP );
  if ( acht == (acolorhash_table) 0 )
    return (acolorhist_vector) 0;
  achv = pam_acolorhashtoacolorhist( acht, maxacolors );
  pam_freeacolorhash( acht );
  return achv;
}



static acolorhash_table
pam_computeacolorhash( apixels, cols, rows, maxacolors, acolorsP )
rgbaPixel** apixels;
int cols, rows, maxacolors;
int* acolorsP;
{
  acolorhash_table acht;
  register rgbaPixel* pP;
  acolorhist_list achl;
  int col, row, hash;

  acht = pam_allocacolorhash( );
  *acolorsP = 0;

  /* Go through the entire image, building a hash table of colors. */
  for ( row = 0; row < rows; ++row )
    for ( col = 0, pP = apixels[row]; col < cols; ++col, ++pP ) {
      hash = pam_hashapixel( *pP );
      for ( achl = acht[hash]; achl != (acolorhist_list) 0; achl = achl->next )
        if ( PAM_EQUAL( achl->ch.acolor, *pP ) )
          break;
      if ( achl != (acolorhist_list) 0 )
        ++(achl->ch.value);
      else {
        if ( ++(*acolorsP) > maxacolors ) {
          pam_freeacolorhash( acht );
          return (acolorhash_table) 0;
        }
        achl = (acolorhist_list) malloc( sizeof(struct acolorhist_list_item) );
        if ( achl == 0 ) {
          fprintf( stderr, "  out of memory computing hash table\n" );
          exit(7);
        }
        achl->ch.acolor = *pP;
        achl->ch.value = 1;
        achl->next = acht[hash];
        acht[hash] = achl;
      }
    }

  return acht;
}



static acolorhash_table
pam_allocacolorhash( )
{
  acolorhash_table acht;
  int i;

  acht = (acolorhash_table) malloc( HASH_SIZE * sizeof(acolorhist_list) );
  if ( acht == 0 ) {
    fprintf( stderr, "  out of memory allocating hash table\n" );
    exit(8);
  }

  for ( i = 0; i < HASH_SIZE; ++i )
    acht[i] = (acolorhist_list) 0;

  return acht;
}



static int
pam_addtoacolorhash( acht, acolorP, value )
acolorhash_table acht;
rgbaPixel* acolorP;
int value;
{
  register int hash;
  register acolorhist_list achl;

  achl = (acolorhist_list) malloc( sizeof(struct acolorhist_list_item) );
  if ( achl == 0 )
    return -1;
  hash = pam_hashapixel( *acolorP );
  achl->ch.acolor = *acolorP;
  achl->ch.value = value;
  achl->next = acht[hash];
  acht[hash] = achl;
  return 0;
}



static acolorhist_vector
pam_acolorhashtoacolorhist( acht, maxacolors )
acolorhash_table acht;
int maxacolors;
{
  acolorhist_vector achv;
  acolorhist_list achl;
  int i, j;

  /* Now collate the hash table into a simple acolorhist array. */
  achv = (acolorhist_vector) malloc( maxacolors * sizeof(struct acolorhist_item) );
  /* (Leave room for expansion by caller.) */
  if ( achv == (acolorhist_vector) 0 ) {
    fprintf( stderr, "  out of memory generating histogram\n" );
    exit(9);
  }

  /* Loop through the hash table. */
  j = 0;
  for ( i = 0; i < HASH_SIZE; ++i )
    for ( achl = acht[i]; achl != (acolorhist_list) 0; achl = achl->next ) {
      /* Add the new entry. */
      achv[j] = achl->ch;
      ++j;
    }

  /* All done. */
  return achv;
}



static int
pam_lookupacolor( acht, acolorP )
acolorhash_table acht;
rgbaPixel* acolorP;
{
  int hash;
  acolorhist_list achl;

  hash = pam_hashapixel( *acolorP );
  for ( achl = acht[hash]; achl != (acolorhist_list) 0; achl = achl->next )
    if ( PAM_EQUAL( achl->ch.acolor, *acolorP ) )
      return achl->ch.value;

  return -1;
}



static void
pam_freeacolorhist( achv )
acolorhist_vector achv;
{
  free( (char*) achv );
}



static void
pam_freeacolorhash( acht )
acolorhash_table acht;
{
  int i;
  acolorhist_list achl, achlnext;

  for ( i = 0; i < HASH_SIZE; ++i )
    for ( achl = acht[i]; achl != (acolorhist_list) 0; achl = achlnext ) {
      achlnext = achl->next;
      free( (char*) achl );
    }
  free( (char*) acht );
}

/** \endcond DONOTDOCUMENT */

int _mapcache_imageio_remap_palette(unsigned char *pixels, int npixels,
                                    rgbaPixel *palette, int numPaletteEntries, unsigned int maxval,
                                    rgbPixel *rgb, unsigned char *a, int *num_a)
{
  int bot_idx, top_idx, x;
  int remap[256];
  /*
   ** remap the palette colors so that all entries with
   ** the maximal alpha value (i.e., fully opaque) are at the end and can
   ** therefore be omitted from the tRNS chunk.  Note that the ordering of
   ** opaque entries is reversed from how Step 3 arranged them--not that
   ** this should matter to anyone.
   */

  for (top_idx = numPaletteEntries-1, bot_idx = x = 0;  x < numPaletteEntries;  ++x) {
    if (palette[x].a == maxval)
      remap[x] = top_idx--;
    else
      remap[x] = bot_idx++;
  }
  /* sanity check:  top and bottom indices should have just crossed paths */
  if (bot_idx != top_idx + 1) {
    return MAPCACHE_FAILURE;
  }

  *num_a = bot_idx;

  for(x=0; x<npixels; x++)
    pixels[x] = remap[pixels[x]];

  for (x = 0; x < numPaletteEntries; ++x) {
    if(maxval == 255) {
      a[remap[x]] = palette[x].a;
      if(palette[x].a == 255) {
        rgb[remap[x]].r = palette[x].r;
        rgb[remap[x]].g = palette[x].g;
        rgb[remap[x]].b = palette[x].b;
      } else if(palette[x].a == 0) {
        rgb[remap[x]].r = 0;
        rgb[remap[x]].g = 0;
        rgb[remap[x]].b = 0;
      } else {
        /* un-premultiply the palette entries */
        rgb[remap[x]].r = (palette[x].r * 255 + palette[x].a / 2) / palette[x].a ;
        rgb[remap[x]].g = (palette[x].g * 255 + palette[x].a / 2) / palette[x].a ;
        rgb[remap[x]].b = (palette[x].b * 255 + palette[x].a / 2) / palette[x].a ;
      }
    } else {
      /* un-scale and un-premultiply the palette entries */
      unsigned char al = a[remap[x]] = (palette[x].a * 255 + (maxval >> 1)) / maxval;
      if(al == 255) {
        rgb[remap[x]].r = (palette[x].r * 255 + (maxval >> 1)) / maxval;
        rgb[remap[x]].g = (palette[x].g * 255 + (maxval >> 1)) / maxval;
        rgb[remap[x]].b = (palette[x].b * 255 + (maxval >> 1)) / maxval;
      } else if(al == 0) {
        rgb[remap[x]].r = rgb[remap[x]].g = rgb[remap[x]].b = 0;

      } else {
        rgb[remap[x]].r = (((palette[x].r * 255 + (maxval >> 1)) / maxval) * 255 + al / 2) / al;
        rgb[remap[x]].g = (((palette[x].g * 255 + (maxval >> 1)) / maxval) * 255 + al / 2) / al;
        rgb[remap[x]].b = (((palette[x].b * 255 + (maxval >> 1)) / maxval) * 255 + al / 2) / al;
      }
    }
  }
  return MAPCACHE_SUCCESS;
}

/**
 * \brief encode an image to quantized PNG format
 * \private \memberof mapcache_image_format_png_q
 * \sa mapcache_image_format::write()
 */
mapcache_buffer* _mapcache_imageio_png_q_encode( mapcache_context *ctx, mapcache_image *image,
    mapcache_image_format *format)
{
  mapcache_buffer *buffer = mapcache_buffer_create(3000,ctx->pool);
  mapcache_image_format_png_q *f = (mapcache_image_format_png_q*)format;
  int compression = f->format.compression_level;
  unsigned int numPaletteEntries = f->ncolors;
  unsigned char *pixels = (unsigned char*)apr_pcalloc(ctx->pool,image->w*image->h*sizeof(unsigned char));
  rgbaPixel palette[256];
  unsigned int maxval;
  png_infop info_ptr;
  rgbPixel rgb[256];
  unsigned char a[256];
  int num_a;
  int row,sample_depth;
  png_structp png_ptr;

  if(MAPCACHE_SUCCESS != _mapcache_imageio_quantize_image(image,&numPaletteEntries,palette, &maxval, NULL, 0)) {
    ctx->set_error(ctx,500,"failed to quantize image buffer");
    return NULL;
  }
  if(MAPCACHE_SUCCESS != _mapcache_imageio_classify(image,pixels,palette,numPaletteEntries)) {
    ctx->set_error(ctx,500,"failed to quantize image buffer");
    return NULL;
  }


  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,NULL,NULL);

  if (!png_ptr)
    return (NULL);

  if(compression == MAPCACHE_COMPRESSION_BEST)
    png_set_compression_level (png_ptr, Z_BEST_COMPRESSION);
  else if(compression == MAPCACHE_COMPRESSION_FAST)
    png_set_compression_level (png_ptr, Z_BEST_SPEED);
  else if(compression == MAPCACHE_COMPRESSION_DISABLE)
    png_set_compression_level (png_ptr, Z_NO_COMPRESSION);
  png_set_filter(png_ptr,0,PNG_FILTER_NONE);
  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr,(png_infopp)NULL);
    return (NULL);
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return (NULL);
  }

  png_set_write_fn(png_ptr,buffer, _mapcache_imageio_png_write_func, _mapcache_imageio_png_flush_func);


  if (numPaletteEntries <= 2)
    sample_depth = 1;
  else if (numPaletteEntries <= 4)
    sample_depth = 2;
  else if (numPaletteEntries <= 16)
    sample_depth = 4;
  else
    sample_depth = 8;

  png_set_IHDR(png_ptr, info_ptr, image->w , image->h,
               sample_depth, PNG_COLOR_TYPE_PALETTE,
               0, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  _mapcache_imageio_remap_palette(pixels, image->w * image->h, palette, numPaletteEntries,
                                  maxval,rgb,a,&num_a);

  png_set_PLTE(png_ptr, info_ptr, (png_colorp)(rgb),numPaletteEntries);
  if(num_a)
    png_set_tRNS(png_ptr, info_ptr, a,num_a, NULL);

  png_write_info(png_ptr, info_ptr);
  png_set_packing(png_ptr);

  for(row=0; row<image->h; row++) {
    unsigned char *rowptr = &(pixels[row*image->w]);
    png_write_row(png_ptr, rowptr);
  }
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);

  return buffer;
}

static mapcache_buffer* _mapcache_imageio_png_create_empty(mapcache_context *ctx, mapcache_image_format *format,
    size_t width, size_t height, unsigned int color)
{
  int i;

  mapcache_image *empty;
  apr_pool_t *pool = NULL;
  mapcache_buffer *buf;
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


mapcache_image_format* mapcache_imageio_create_png_format(apr_pool_t *pool, char *name, mapcache_compression_type compression)
{
  mapcache_image_format_png *format = apr_pcalloc(pool, sizeof(mapcache_image_format_png));
  format->format.name = name;
  format->format.extension = apr_pstrdup(pool,"png");
  format->format.mime_type = apr_pstrdup(pool,"image/png");
  format->compression_level = compression;
  format->format.metadata = apr_table_make(pool,3);
  format->format.write = _mapcache_imageio_png_encode;
  format->format.create_empty_image = _mapcache_imageio_png_create_empty;
  format->format.type = GC_PNG;
  return (mapcache_image_format*)format;
}

mapcache_image_format* mapcache_imageio_create_png_q_format(apr_pool_t *pool, char *name, mapcache_compression_type compression, int ncolors)
{
  mapcache_image_format_png_q *format = apr_pcalloc(pool, sizeof(mapcache_image_format_png_q));
  format->format.format.name = name;
  format->format.format.extension = apr_pstrdup(pool,"png");
  format->format.format.mime_type = apr_pstrdup(pool,"image/png");
  format->format.compression_level = compression;
  format->format.format.write = _mapcache_imageio_png_q_encode;
  format->format.format.create_empty_image = _mapcache_imageio_png_create_empty;
  format->format.format.metadata = apr_table_make(pool,3);
  format->ncolors = ncolors;
  format->format.format.type = GC_PNG;
  return (mapcache_image_format*)format;
}

/** @} */

/* vim: ts=2 sts=2 et sw=2
*/
