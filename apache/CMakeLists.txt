option(WITH_VERSION_STRING "Show MapCache in server version string" ON)
option(WITH_APACHE "Build Apache Module" ON)

if(WITH_APACHE)
  add_library(mod_mapcache MODULE mod_mapcache.c)
  set_target_properties(mod_mapcache PROPERTIES PREFIX "")
  target_link_libraries(mod_mapcache mapcache)
  
  find_package(APACHE)
  if(APACHE_FOUND)
     include_directories(${APACHE_INCLUDE_DIR})
     target_link_libraries(mod_mapcache ${APACHE_LIBRARY})
  else(APACHE_FOUND)
     report_mandatory_not_found(APACHE)
  endif(APACHE_FOUND)
  
  if(WITH_VERSION_STRING)
    set(USE_VERSION_STRING 1)
  endif(WITH_VERSION_STRING)
  
  
  
  configure_file (
    "${PROJECT_SOURCE_DIR}/apache/mod_mapcache-config.h.in"
    "${PROJECT_BINARY_DIR}/apache/mod_mapcache-config.h"
    )
  include_directories("${PROJECT_BINARY_DIR}/apache/")
  
  IF(APPLE)
  set_target_properties(mod_mapcache PROPERTIES
             LINK_FLAGS "-Wl,-flat_namespace -Wl,-undefined -Wl,suppress")
  ENDIF(APPLE)

  message(STATUS "* Apache Module support status:")
  status_optional_component("Mapcache Version String" "${USE_VERSION_STRING}" "mod_mapcache/${MAPCACHE_VERSION_STRING}")
  if(APACHE_MODULE_DIR)
     message(STATUS "  * Module will be installed to : ${APACHE_MODULE_DIR}")
  else(APACHE_MODULE_DIR)
     message(WARNING "  * Module will not be automatically installed: module directory not found")
  endif(APACHE_MODULE_DIR)
  
  
  if(APACHE_MODULE_DIR)
    INSTALL(TARGETS mod_mapcache DESTINATION ${APACHE_MODULE_DIR})
  endif(APACHE_MODULE_DIR)
else(WITH_APACHE)
   message(STATUS " * Apache Module support status: DISABLED")
endif(WITH_APACHE)
