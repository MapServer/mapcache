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

int _mapcache_imageio_quantize_image(mapcache_image *rb,
                                     unsigned int *reqcolors, rgbaPixel *palette,
                                     unsigned int *maxval,
                                     rgbaPixel *forced_palette, int num_forced_palette_entries);
int _mapcache_imageio_classify(mapcache_image *rb, unsigned char *pixels,
                               rgbaPixel *palette, int numPaletteEntries);

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
/*
typedef struct {
  unsigned char b,g,r,a;
} rgbaPixel;

typedef struct {
  unsigned char r,g,b;
} rgbPixel;
*/


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

acolorhist_vector mediancut(acolorhist_vector achv, int colors, int sum, unsigned char maxval, int newcolors);
int redcompare (const void *ch1, const void *ch2);
int greencompare (const void *ch1, const void *ch2);
int bluecompare (const void *ch1, const void *ch2);
int alphacompare (const void *ch1, const void *ch2);
int sumcompare (const void *b1, const void *b2);

acolorhist_vector pam_acolorhashtoacolorhist
(acolorhash_table acht, int maxacolors);
acolorhist_vector pam_computeacolorhist
(rgbaPixel **apixels, int cols, int rows, int maxacolors, int* acolorsP);
acolorhash_table pam_computeacolorhash
(rgbaPixel** apixels, int cols, int rows, int maxacolors, int* acolorsP);
acolorhash_table pam_allocacolorhash (void);
int pam_addtoacolorhash
(acolorhash_table acht, rgbaPixel *acolorP, int value);
int pam_lookupacolor (acolorhash_table acht, rgbaPixel* acolorP);
void pam_freeacolorhist (acolorhist_vector achv);
void pam_freeacolorhash (acolorhash_table acht);



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

/** \endcond DONOTDOCUMENT */
