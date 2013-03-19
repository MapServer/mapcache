find_path(GEOTIFF_INCLUDE_DIR
   NAMES geotiff.h
   PATH_SUFFIXES libgeotiff geotiff)

find_library(GEOTIFF_LIBRARY
   NAMES geotiff geotiff3
   PATH_SUFFIXES geotiff )

set(GEOTIFF_INCLUDE_DIRS ${GEOTIFF_INCLUDE_DIR})
set(GEOTIFF_LIBRARIES ${GEOTIFF_LIBRARY})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GEOTIFF DEFAULT_MSG GEOTIFF_LIBRARY GEOTIFF_INCLUDE_DIR)
mark_as_advanced(GEOTIFF_LIBRARY GEOTIFF_INCLUDE_DIR)
