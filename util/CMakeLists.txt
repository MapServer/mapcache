option(WITH_GEOS "Choose if GEOS geometry operations support should be built in" ON)
option(WITH_OGR "Choose if OGR/GDAL input vector support should be built in" ON)

add_executable(mapcache_seed mapcache_seed.c)
target_link_libraries(mapcache_seed mapcache)

if(WITH_OGR)
  find_package(GDAL)
  if(GDAL_FOUND)
    include_directories(${GDAL_INCLUDE_DIR})
    target_link_libraries(mapcache_seed ${GDAL_LIBRARY})
    set (USE_OGR 1)
  else(GDAL_FOUND)
    #TODO:message here could be more precise: we need the GDAL library but we don't need to force gdal support
    report_dependency_error(GDAL OGR)
  endif(GDAL_FOUND)
endif(WITH_OGR)


if(WITH_GEOS)
  find_package(GEOS)
  if(GEOS_FOUND)
    include_directories(${GEOS_INCLUDE_DIR})
    target_link_libraries(mapcache_seed ${GEOS_LIBRARY})
    set (USE_GEOS 1)
  else(GEOS_FOUND)
    report_optional_not_found(GEOS)
  endif(GEOS_FOUND)
endif (WITH_GEOS)

  
configure_file (
  "${PROJECT_SOURCE_DIR}/util/mapcache-util-config.h.in"
  "${PROJECT_BINARY_DIR}/util/mapcache-util-config.h"
  )
include_directories("${PROJECT_BINARY_DIR}/util/")


message(STATUS "* Seeder Configuration Options:")
status_optional_component("GEOS" "${USE_GEOS}" "${GEOS_LIBRARY}")
status_optional_component("OGR" "${USE_OGR}" "${GDAL_LIBRARY}")

INSTALL(TARGETS mapcache_seed RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
