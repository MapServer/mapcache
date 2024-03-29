set(BERKELEYDB_FOUND TRUE)

# set the search path

if (WIN32)
    file(GLOB BERKELEYDB_SEARCH_PATH "C:/Program Files/Oracle/Berkeley DB*")
    if (NOT BERKELEYDB_SEARCH_PATH)
    file(GLOB BERKELEYDB_SEARCH_PATH "C:/Program Files (x86)/Oracle/Berkeley DB*")
    endif (NOT BERKELEYDB_SEARCH_PATH)
else (WIN32)
   file(GLOB BERKELEYDB_SEARCH_PATH "/usr/local/BerkeleyDB*")
   file(GLOB BERKELEYDB_INC_SEARCH_PATH "/usr/local/BerkeleyDB*/include")
   file(GLOB BERKELEYDB_LIB_SEARCH_PATH "/usr/local/BerkeleyDB*/lib")
endif (WIN32)

# search for header
find_path(BERKELEYDB_INCLUDE_DIR
          NAMES "db.h"
          HINTS ${BERKELEYDB_SEARCH_PATH}
          ${BERKELEYDB_INC_SEARCH_PATH}
          )

# header is found

if (BERKELEYDB_INCLUDE_DIR)

    # retrieve version information from the header
    file(READ "${BERKELEYDB_INCLUDE_DIR}/db.h" DB_H_FILE)

    string(REGEX REPLACE ".*#define[ \t]+DB_VERSION_STRING[ \t]+\"([^\"]+)\".*" "\\1" BERKELEYDB_VERSION       "${DB_H_FILE}")
    string(REGEX REPLACE ".*#define[ \t]+DB_VERSION_MAJOR[ \t]+([0-9]+).*"      "\\1" BERKELEYDB_VERSION_MAJOR "${DB_H_FILE}")
    string(REGEX REPLACE ".*#define[ \t]+DB_VERSION_MINOR[ \t]+([0-9]+).*"      "\\1" BERKELEYDB_VERSION_MINOR "${DB_H_FILE}")
    string(REGEX REPLACE ".*#define[ \t]+DB_VERSION_PATCH[ \t]+([0-9]+).*"      "\\1" BERKELEYDB_VERSION_PATCH "${DB_H_FILE}")

    # search for library
    if (WIN32)
        file(GLOB BERKELEYDB_LIBRARIES
             "${DBROOTDIR}/lib/libdb${BERKELEYDB_VERSION_MAJOR}${BERKELEYDB_VERSION_MINOR}.lib"
             "${BERKELEYDB_SEARCH_PATH}/lib/libdb${BERKELEYDB_VERSION_MAJOR}${BERKELEYDB_VERSION_MINOR}.lib")

    else (WIN32)
        find_library(BERKELEYDB_LIBRARY
                     NAMES "db-${BERKELEYDB_VERSION_MAJOR}.${BERKELEYDB_VERSION_MINOR}" db
                     HINTS ${BERKELEYDB_SEARCH_PATH}
                           ${BERKELEYDB_LIB_SEARCH_PATH}
                  )
    endif (WIN32)

endif (BERKELEYDB_INCLUDE_DIR)

# header is not found

if (NOT BERKELEYDB_INCLUDE_DIR OR NOT BERKELEYDB_LIBRARY)
   set(BERKELEYDB_FOUND_TMP FALSE)
else (NOT BERKELEYDB_INCLUDE_DIR OR NOT BERKELEYDB_LIBRARY)
   set(BERKELEYDB_FOUND_TMP TRUE)
endif (NOT BERKELEYDB_INCLUDE_DIR OR NOT BERKELEYDB_LIBRARY)

# check found version

if (BERKELEYDB_FIND_VERSION AND BERKELEYDB_FOUND_TMP)

    set(BERKELEYDB_FOUND_VERSION "${BERKELEYDB_VERSION_MAJOR}.${BERKELEYDB_VERSION_MINOR}.${BERKELEYDB_VERSION_PATCH}")

    if (BERKELEYDB_FIND_VERSION_EXACT)
        if (NOT ${BERKELEYDB_FOUND_VERSION} VERSION_EQUAL ${BERKELEYDB_FIND_VERSION})
           set(BERKELEYDB_FOUND_TMP FALSE)
        endif (NOT ${BERKELEYDB_FOUND_VERSION} VERSION_EQUAL ${BERKELEYDB_FIND_VERSION})
    else (BERKELEYDB_FIND_VERSION_EXACT)
        if (${BERKELEYDB_FOUND_VERSION} VERSION_LESS ${BERKELEYDB_FIND_VERSION})
           set(BERKELEYDB_FOUND_TMP FALSE)
        endif (${BERKELEYDB_FOUND_VERSION} VERSION_LESS ${BERKELEYDB_FIND_VERSION})
    endif (BERKELEYDB_FIND_VERSION_EXACT)

    if (NOT BERKELEYDB_FOUND_TMP)
       message(SEND_ERROR "Berkeley DB library found, but with wrong version v${BERKELEYDB_FIND_VERSION} (${BERKELEYDB_FOUND_VERSION} was found)")
       unset(BERKELEYDB_INCLUDE_DIR)
       unset(BERKELEYDB_LIBRARY)
    endif (NOT BERKELEYDB_FOUND_TMP)

endif (BERKELEYDB_FIND_VERSION AND BERKELEYDB_FOUND_TMP)

set(BERKELEYDB_INCLUDE_DIRS ${BERKELEYDB_INCLUDE_DIR})
set(BERKELEYDB_LIBRARIES ${BERKELEYDB_LIBRARY})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BerkeleyDB DEFAULT_MSG BERKELEYDB_LIBRARY BERKELEYDB_INCLUDE_DIR)
mark_as_advanced(BERKELEYDB_LIBRARY BERKELEYDB_INCLUDE_DIR)
