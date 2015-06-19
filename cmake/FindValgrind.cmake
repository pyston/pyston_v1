# find valgrind header and binary
include(FindPackageHandleStandardArgs)

find_path(VALGRIND_INCLUDE_DIR
          NAMES valgrind.h
          PATHS /usr/include /usr/include/valgrind ${VALGRIND_DIR}/include/valgrind)

find_program(VALGRIND_BIN NAMES valgrind PATHS /usr/bin ${VALGRIND_DIR}/bin)

find_package_handle_standard_args(valgrind REQUIRED_VARS VALGRIND_BIN VALGRIND_INCLUDE_DIR)
