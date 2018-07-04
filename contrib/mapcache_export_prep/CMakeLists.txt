option(WITH_GEOS
       "Choose if GEOS geometry operations support should be built in" ON)
option(WITH_OGR
       "Choose if OGR/GDAL input vector support should be built in" ON)

add_executable(mapcache_export_prep mapcache_export_prep.c)
target_link_libraries(mapcache_export_prep mapcache)

if (WITH_OGR)
  find_package(GDAL)
  if (GDAL_FOUND)
    include_directories(${GDAL_INCLUDE_DIR})
    target_link_libraries(mapcache_export_prep ${GDAL_LIBRARY})
    set (USE_OGR 1)
  else (GDAL_FOUND)
    report_optional_not_found(GDAL)
  endif (GDAL_FOUND)
endif (WITH_OGR)

if (WITH_GEOS)
  find_package(GEOS)
  if (GEOS_FOUND)
    include_directories(${GEOS_INCLUDE_DIR})
    target_link_libraries(mapcache_export_prep ${GEOS_LIBRARY})
    set (USE_GEOS 1)
  else (GEOS_FOUND)
    report_optional_not_found(GEOS)
  endif (GEOS_FOUND)
endif (WITH_GEOS)


set (CURRENT_BINARY_DIR "${PROJECT_BINARY_DIR}/contrib/mapcache_export_prep/")
include_directories("${CURRENT_BINARY_DIR}")

configure_file (
  "${CMAKE_CURRENT_SOURCE_DIR}/mapcache_export_prep_config.h.in"
  "${CURRENT_BINARY_DIR}/mapcache_export_prep_config.h"
)

message(STATUS "* Export Preparation Configuration Options:")
status_optional_component("GEOS" "${USE_GEOS}" "${GEOS_LIBRARY}")
status_optional_component("OGR" "${USE_OGR}" "${GDAL_LIBRARY}")

INSTALL(TARGETS mapcache_export_prep
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
