option(WITH_GEOS
       "Choose if GEOS geometry operations support should be built in" ON)
option(WITH_OGR
       "Choose if OGR/GDAL input vector support should be built in" ON)

add_executable(mapcache_detail mapcache_detail.c)
target_link_libraries(mapcache_detail mapcache)

if (WITH_SQLITE)
  find_package(SQLITE)
  if(SQLITE_FOUND)
    include_directories(${SQLITE_INCLUDE_DIR})
    target_link_libraries(mapcache_detail ${SQLITE_LIBRARY})
  else(SQLITE_FOUND)
    report_mandatory_not_found(SQLITE)
  endif(SQLITE_FOUND)
else (WITH_SQLITE)
  report_dependency_error(MAPCACHE_DETAIL SQLITE)
endif (WITH_SQLITE)

if (WITH_OGR)
  find_package(GDAL)
  if (GDAL_FOUND)
    include_directories(${GDAL_INCLUDE_DIR})
    target_link_libraries(mapcache_detail ${GDAL_LIBRARY})
    set (USE_OGR 1)
  else (GDAL_FOUND)
    report_optional_not_found(GDAL)
  endif (GDAL_FOUND)
endif (WITH_OGR)

if (WITH_GEOS)
  find_package(GEOS)
  if (GEOS_FOUND)
    include_directories(${GEOS_INCLUDE_DIR})
    target_link_libraries(mapcache_detail ${GEOS_LIBRARY})
    set (USE_GEOS 1)
  else (GEOS_FOUND)
    report_optional_not_found(GEOS)
  endif (GEOS_FOUND)
endif (WITH_GEOS)


set (CURRENT_BINARY_DIR "${PROJECT_BINARY_DIR}/contrib/mapcache_detail/")
include_directories("${CURRENT_BINARY_DIR}")

configure_file (
  "${CMAKE_CURRENT_SOURCE_DIR}/mapcache_detail_config.h.in"
  "${CURRENT_BINARY_DIR}/mapcache_detail_config.h"
)

message(STATUS "* mapcache_detail Configuration Options:")
message(STATUS " * Mandatory components")
message(STATUS "  * SQLite: ${SQLITE_LIBRARY}")
message(STATUS " * Optional components")
status_optional_component("GEOS" "${USE_GEOS}" "${GEOS_LIBRARY}")
status_optional_component("OGR" "${USE_OGR}" "${GDAL_LIBRARY}")

INSTALL(TARGETS mapcache_detail
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
