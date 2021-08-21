/******************************************************************************
 *
 * Project:  MapCache
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2016 Regents of the University of Minnesota.
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

/*! \file mapcache_services.h
    \brief structure declarations for supported services
 */


#ifndef MAPCACHE_SERVICES_H_
#define MAPCACHE_SERVICES_H_

#include "mapcache.h"


/** \addtogroup services */
/** @{ */

typedef struct mapcache_request_get_capabilities_wmts mapcache_request_get_capabilities_wmts;
typedef struct mapcache_service_wmts mapcache_service_wmts;

struct mapcache_request_get_capabilities_wmts {
  mapcache_request_get_capabilities request;
};

/**\class mapcache_service_wmts
 * \brief a WMTS service
 * \implements mapcache_service
 */
struct mapcache_service_wmts {
  mapcache_service service;
};



typedef struct mapcache_service_wms mapcache_service_wms;
typedef struct mapcache_request_get_capabilities_wms mapcache_request_get_capabilities_wms;

struct mapcache_request_get_capabilities_wms {
  mapcache_request_get_capabilities request;
};

/**\class mapcache_service_wms
 * \brief an OGC WMS service
 * \implements mapcache_service
 */
struct mapcache_service_wms {
  mapcache_service service;
  int maxsize;
  apr_array_header_t *forwarding_rules;
  mapcache_getmap_strategy getmap_strategy;
  mapcache_resample_mode resample_mode;
  mapcache_image_format *getmap_format;
  int allow_format_override; /* can the client specify which image format should be returned */
};

typedef struct mapcache_service_ve mapcache_service_ve;

/**\class mapcache_service_ve
 * \brief a virtualearth service
 * \implements mapcache_service
 */
struct mapcache_service_ve {
  mapcache_service service;
};


typedef struct mapcache_service_gmaps mapcache_service_gmaps;
typedef struct mapcache_service_tms mapcache_service_tms;
typedef struct mapcache_request_get_capabilities_tms mapcache_request_get_capabilities_tms;

/**\class mapcache_service_tms
 * \brief a TMS service
 * \implements mapcache_service
 */
struct mapcache_service_tms {
  mapcache_service service;
  int reverse_y;
};

struct mapcache_request_get_capabilities_tms {
  mapcache_request_get_capabilities request;
  mapcache_tileset *tileset;
  mapcache_grid_link *grid_link;
  char *version;
};


typedef struct mapcache_service_mapguide mapcache_service_mapguide;

struct mapcache_service_mapguide {
  mapcache_service service;
  int rows_per_folder;
  int cols_per_folder;
};



typedef struct mapcache_service_kml mapcache_service_kml;
typedef struct mapcache_request_get_capabilities_kml mapcache_request_get_capabilities_kml;
struct mapcache_request_get_capabilities_kml {
  mapcache_request_get_capabilities request;
  mapcache_tile *tile;
  mapcache_tileset *tileset;
  mapcache_grid_link *grid;
};
/**\class mapcache_service_kml
 * \brief a KML superoverlay service
 * \implements mapcache_service
 */
struct mapcache_service_kml {
  mapcache_service service;
};

typedef struct mapcache_request_get_capabilities_demo mapcache_request_get_capabilities_demo;
typedef struct mapcache_service_demo mapcache_service_demo;

/**
 * the capabilities request for a specific service, to be able to create
 * demo pages specific to a given service
 */
struct mapcache_request_get_capabilities_demo {
  mapcache_request_get_capabilities request;
  mapcache_service *service;
};

/**\class mapcache_service_demo
 * \brief a demo service
 * \implements mapcache_service
 */
struct mapcache_service_demo {
  mapcache_service service;

};




#endif /*MAPCACHE_SERVICES_H*/
