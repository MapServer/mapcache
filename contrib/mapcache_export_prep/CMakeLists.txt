add_executable(mapcache_export_prep mapcache_export_prep.c)
target_link_libraries(mapcache_export_prep mapcache)
include_directories("${PROJECT_BINARY_DIR}/contrib/mapcache_export_prep/")
INSTALL(TARGETS mapcache_export_prep RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
