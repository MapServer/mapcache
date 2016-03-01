/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: GDAL datasource support (incomplete and disabled)
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
#include "ezxml.h"
#include <apr_tables.h>
#include <apr_strings.h>

#ifdef USE_GDAL

#include <gdal.h>
#include <cpl_conv.h>

#include "gdal_alg.h"
#include "cpl_string.h"
#include "ogr_srs_api.h"

typedef struct mapcache_source_gdal mapcache_source_gdal;

/**\class mapcache_source_gdal
 * \brief GDAL mapcache_source
 * \implements mapcache_source
 */
struct mapcache_source_gdal {
  mapcache_source source;
  char *datastr; /**< the gdal source string*/
  apr_table_t *gdal_params; /**< GDAL parameters specified in configuration */
  GDALDatasetH *poDataset;
};

/************************************************************************/
/*                        GDALWarpCreateOutput()                        */
/*                                                                      */
/*      Create the output file based on various commandline options,    */
/*      and the input file.                                             */
/************************************************************************/

static GDALDatasetH
GDALWarpCreateOutput( GDALDatasetH hSrcDS, const char *pszSourceSRS, const char *pszTargetSRS,
                      int width, int height, mapcache_extent *extent)
{
    GDALDriverH hDriver;
    GDALDatasetH hDstDS;
    void *hTransformArg;
    double adfDstGeoTransform[6];
    double res_x, res_y;
    int nPixels,nLines;

    hDriver = GDALGetDriverByName( "MEM" );

/* -------------------------------------------------------------------- */
/*      Create a transformation object from the source to               */
/*      destination coordinate system.                                  */
/* -------------------------------------------------------------------- */
    hTransformArg =
        GDALCreateGenImgProjTransformer( hSrcDS, pszSourceSRS,
                                         NULL, pszTargetSRS,
                                         TRUE, 1000.0, 0 );

    if( hTransformArg == NULL )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Get approximate output definition.                              */
/* -------------------------------------------------------------------- */
    if( GDALSuggestedWarpOutput( hSrcDS,
                                 GDALGenImgProjTransform, hTransformArg,
                                 adfDstGeoTransform, &nPixels, &nLines )
        != CE_None )
        return NULL;

    GDALDestroyGenImgProjTransformer( hTransformArg );

    res_x = (extent->maxx - extent->minx) / width;
    res_y = (extent->maxy - extent->miny) / height;

    adfDstGeoTransform[0] = extent->minx;
    adfDstGeoTransform[3] = extent->maxy;
    adfDstGeoTransform[1] = res_x;
    adfDstGeoTransform[5] = -res_y;


    hDstDS = GDALCreate( hDriver, "temp_gdal", width, height, 4, GDT_Byte, NULL);

    if( hDstDS == NULL )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Write out the projection definition.                            */
/* -------------------------------------------------------------------- */
    GDALSetProjection( hDstDS, pszTargetSRS );
    GDALSetGeoTransform( hDstDS, adfDstGeoTransform );

    return hDstDS;
}

/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::render_metatile()
 */
void _mapcache_source_gdal_render_metatile(mapcache_context *ctx, mapcache_map *map)
{
  mapcache_source_gdal *gdal = (mapcache_source_gdal*)map->tileset->source;
  GDALDatasetH  hSrcDS,hDstDS;
  OGRSpatialReferenceH hDstSRS;
  char *src_srs,*dst_srs;
  mapcache_buffer *data;
  char **papszWarpOptions = NULL;
  void *hTransformArg, *hGenImgProjArg=NULL;
  GDALTransformerFunc pfnTransformer = NULL;
  GDALRasterBandH *redband, *greenband, *blueband, *alphaband;
  unsigned char *rasterdata;

  CPLErrorReset();

  /* -------------------------------------------------------------------- */
  /*      Open source dataset.                                            */
  /* -------------------------------------------------------------------- */
  hSrcDS = GDALOpen( gdal->datastr, GA_ReadOnly );

  if( hSrcDS == NULL )
    exit( 2 );

  /* -------------------------------------------------------------------- */
  /*      Check that there's at least one raster band                     */
  /* -------------------------------------------------------------------- */
  if ( GDALGetRasterCount(hSrcDS) == 0 ) {
    ctx->set_error(ctx, 500, "Input gdal source for %s has no raster bands.\n", gdal->source.name );
    GDALClose(hSrcDS);
    return;
  }

  if( GDALGetProjectionRef( hSrcDS ) != NULL
      && strlen(GDALGetProjectionRef( hSrcDS )) > 0 ) {
    src_srs = apr_pstrdup(ctx->pool,GDALGetProjectionRef( hSrcDS ));
  } else if( GDALGetGCPProjection( hSrcDS ) != NULL
           && strlen(GDALGetGCPProjection(hSrcDS)) > 0
           && GDALGetGCPCount( hSrcDS ) > 1 ) {
    src_srs = apr_pstrdup(ctx->pool,GDALGetGCPProjection( hSrcDS ));
  } else {
    ctx->set_error(ctx, 500, "Input gdal source for %s has no defined SRS\n", gdal->source.name );
    GDALClose(hSrcDS);
    return;
  }


  hDstSRS = OSRNewSpatialReference( NULL );
  if( OSRSetFromUserInput( hDstSRS, map->grid_link->grid->srs ) == OGRERR_NONE )
    OSRExportToWkt( hDstSRS, &dst_srs );
  else {
    ctx->set_error(ctx,500,"failed to parse gdal srs %s",map->grid_link->grid->srs);
    return;
  }

  OSRDestroySpatialReference( hDstSRS );

  hDstDS = GDALWarpCreateOutput( hSrcDS, src_srs, dst_srs, map->width, map->height, &map->extent);
  papszWarpOptions = CSLSetNameValue( papszWarpOptions, "INIT", "0" );

/* -------------------------------------------------------------------- */
/*      Create a transformation object from the source to               */
/*      destination coordinate system.                                  */
/* -------------------------------------------------------------------- */
  hGenImgProjArg = GDALCreateGenImgProjTransformer( hSrcDS, src_srs,
                                                    hDstDS, dst_srs,
                                                    TRUE, 1000.0, 0 );

  if( hGenImgProjArg == NULL ) {
    ctx->set_error(ctx,500,"failed to GDALCreateGenImgProjTransformer()");
    return;
  }

/* -------------------------------------------------------------------- */
/*      Warp the transformer with a linear approximator                 */
/* -------------------------------------------------------------------- */
  hTransformArg = GDALCreateApproxTransformer( GDALGenImgProjTransform,
                                               hGenImgProjArg, 0.1 );
  pfnTransformer = GDALApproxTransform;

  /* -------------------------------------------------------------------- */
  /*      Now actually invoke the warper to do the work.                  */
  /* -------------------------------------------------------------------- */
  GDALSimpleImageWarp( hSrcDS, hDstDS, 0, NULL,
                       pfnTransformer, hTransformArg,
                       GDALDummyProgress, NULL, papszWarpOptions );


  CSLDestroy( papszWarpOptions );

  GDALDestroyApproxTransformer( hTransformArg );
  GDALDestroyGenImgProjTransformer( hGenImgProjArg );

  if(GDALGetRasterCount(hDstDS) < 3) {
    ctx->set_error(ctx, 500,"gdal did not create a 3/4 band image");
    return;
  }


  redband = GDALGetRasterBand(hDstDS,1);
  greenband = GDALGetRasterBand(hDstDS,2);
  blueband = GDALGetRasterBand(hDstDS,3);
  if(GDALGetRasterCount(hDstDS) > 3) {
    alphaband = GDALGetRasterBand(hDstDS,4);
  } else {
    alphaband = NULL;
  }

  data = mapcache_buffer_create(map->height*map->width*4,ctx->pool);
  rasterdata = data->buf;

  GDALRasterIO(blueband,GF_Read,0,0,map->width,map->height, rasterdata,map->width,map->height,GDT_Byte,4,4*map->width);
  GDALRasterIO(greenband,GF_Read,0,0,map->width,map->height, rasterdata+1,map->width, map->height,GDT_Byte,4,4*map->width);
  GDALRasterIO(redband,GF_Read,0,0,map->width,map->height,rasterdata+2,map->width,map->height,GDT_Byte,4,4*map->width);
  if(GDALGetRasterCount(hDstDS)>=4)
    GDALRasterIO(alphaband,GF_Read,0,0,map->width,map->height,rasterdata+3,map->width,map->height,GDT_Byte,4,4*map->width);
  else {
    unsigned char *alphaptr;
    int i;
    for(alphaptr = rasterdata+3, i=0; i<map->width*map->height; i++, alphaptr+=4) {
      *alphaptr = 255;
    }
  }

  map->raw_image = mapcache_image_create(ctx);
  map->raw_image->w = map->width;
  map->raw_image->h = map->height;
  map->raw_image->stride = map->width * 4;
  map->raw_image->data = rasterdata;
  if(alphaband)
    map->raw_image->has_alpha = MC_ALPHA_UNKNOWN;
  else
    map->raw_image->has_alpha = MC_ALPHA_NO;


  GDALClose( hDstDS );
  GDALClose( hSrcDS);
}

/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_parse()
 */
void _mapcache_source_gdal_configuration_parse(mapcache_context *ctx, ezxml_t node, mapcache_source *source)
{
  ezxml_t cur_node;
  mapcache_source_gdal *src = (mapcache_source_gdal*)source;

  if ((cur_node = ezxml_child(node,"data")) != NULL) {
    src->datastr = apr_pstrdup(ctx->pool,cur_node->txt);
  }

  if ((cur_node = ezxml_child(node,"gdalparams")) != NULL) {
    for(cur_node = cur_node->child; cur_node; cur_node = cur_node->sibling) {
      apr_table_set(src->gdal_params, cur_node->name, cur_node->txt);
    }
  }
}

/**
 * \private \memberof mapcache_source_gdal
 * \sa mapcache_source::configuration_check()
 */
void _mapcache_source_gdal_configuration_check(mapcache_context *ctx, mapcache_cfg *cfg,
    mapcache_source *source)
{
  mapcache_source_gdal *src = (mapcache_source_gdal*)source;
  /* check all required parameters are configured */
  if(!strlen(src->datastr)) {
    ctx->set_error(ctx, 500, "gdal source %s has no data",source->name);
    return;
  }
  src->poDataset = (GDALDatasetH*)GDALOpen(src->datastr,GA_ReadOnly);
  if( src->poDataset == NULL ) {
    ctx->set_error(ctx, 500, "gdalOpen failed on data %s", src->datastr);
    return;
  }

}
#endif //USE_GDAL

mapcache_source* mapcache_source_gdal_create(mapcache_context *ctx)
{
#ifdef USE_GDAL
  mapcache_source_gdal *source = apr_pcalloc(ctx->pool, sizeof(mapcache_source_gdal));
  if(!source) {
    ctx->set_error(ctx, 500, "failed to allocate gdal source");
    return NULL;
  }
  mapcache_source_init(ctx, &(source->source));
  source->source.type = MAPCACHE_SOURCE_GDAL;
  source->source.render_map = _mapcache_source_gdal_render_metatile;
  source->source.configuration_check = _mapcache_source_gdal_configuration_check;
  source->source.configuration_parse_xml = _mapcache_source_gdal_configuration_parse;
  source->gdal_params = apr_table_make(ctx->pool,4);
  GDALAllRegister();
  return (mapcache_source*)source;
#else
  ctx->set_error(ctx, 400, "failed to create gdal source, GDAL support is not compiled in this version");
  return NULL;
#endif
}



/* vim: ts=2 sts=2 et sw=2
*/
