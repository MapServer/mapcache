# Look for the header file.
find_path(GOOGLEPERF_INCLUDE_DIR NAMES gperftools/profiler.h)

# Look for the library.
find_library(GOOGLEPERF_LIBRARY NAMES profiler)

set(GOOGLEPERF_INCLUDE_DIRS ${GOOGLEPERF_INCLUDE_DIR})
set(GOOGLEPERF_LIBRARIES ${GOOGLEPERF_LIBRARY})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GOOGLEPERF DEFAULT_MSG GOOGLEPERF_LIBRARY GOOGLEPERF_INCLUDE_DIR)
mark_as_advanced(GOOGLEPERF_LIBRARY GOOGLEPERF_INCLUDE_DIR)
