if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(EXT_BUILD_FLAGS "-O3 -DNDEBUG")
else()
    set(EXT_BUILD_FLAGS "-g -O0")
endif()

if (${ENABLE_REF_DEBUG})
    set(EXT_BUILD_FLAGS "-DPy_REF_DEBUG -DPYMALLOC_DEBUG -DPy_TRACE_REFS ${EXT_BUILD_FLAGS}")
endif()

configure_file(lib_pyston/_sysconfigdata.py.in lib_pyston/_sysconfigdata.py)
