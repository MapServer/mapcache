option(WITH_CGI "Choose if CGI executable should be built" ON)
option(WITH_FCGI "Choose if CGI executable should support FastCGI" ON)

if(WITH_CGI)
  if(NOT DEFINED CMAKE_INSTALL_CGIBINDIR)
     set(CMAKE_INSTALL_CGIBINDIR ${CMAKE_INSTALL_BINDIR})
  endif(NOT DEFINED CMAKE_INSTALL_CGIBINDIR)

  add_executable(mapcache.fcgi mapcache.c)
  target_link_libraries(mapcache.fcgi mapcache)
  
  if(WITH_FCGI)
    find_package(FCGI)
    if(FCGI_FOUND)
      include_directories(${FCGI_INCLUDE_DIR})
      target_link_libraries(mapcache.fcgi ${FCGI_LIBRARY})
      set (USE_FASTCGI 1)
    else(FCGI_FOUND)
      report_optional_not_found(FCGI)
    endif(FCGI_FOUND)
  endif (WITH_FCGI)
  
configure_file (
  "${PROJECT_SOURCE_DIR}/cgi/mapcache-cgi-config.h.in"
  "${PROJECT_BINARY_DIR}/cgi/mapcache-cgi-config.h"
  )
include_directories("${PROJECT_BINARY_DIR}/cgi/")
  
  message(STATUS "* CGI Configuration Options:")
  status_optional_component("FastCGI" "${USE_FASTCGI}" "${FCGI_LIBRARY}")
  
  INSTALL(TARGETS mapcache.fcgi RUNTIME DESTINATION ${CMAKE_INSTALL_CGIBINDIR})
else(WITH_CGI)
   message(STATUS "* CGI Configuration Options: DISABLED")
endif(WITH_CGI)
