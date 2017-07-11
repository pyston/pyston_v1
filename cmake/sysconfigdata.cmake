if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(EXT_BUILD_FLAGS "-O3 -DNDEBUG")
else()
    set(EXT_BUILD_FLAGS "-g -O0")
endif()

if (${ENABLE_REF_DEBUG})
    set(EXT_BUILD_FLAGS "-DPy_REF_DEBUG -DPYMALLOC_DEBUG -DPy_TRACE_REFS ${EXT_BUILD_FLAGS}")
endif()

configure_file(cmake/_sysconfigdata.py.in lib/python3.5/_sysconfigdata.py)

# CMake sucks: it has no idea that configure-generated files can be installed.
# Just tell it to install whatever file is at that particular location, and rely on
# the rest of the build rules to ensure that it's made in time.
install(FILES ${CMAKE_BINARY_DIR}/lib/python3.5/_sysconfigdata.py DESTINATION lib/python3.5)
