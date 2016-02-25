set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-undef")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_definitions(-DGTEST_HAS_RTTI=0 ${LLVM_DEFINITIONS})
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${LLVM_SOURCE_DIR}/utils/unittest/googletest/include)

add_custom_target(unittests)

macro(add_unittest unittest)
  add_executable(${unittest}_unittest EXCLUDE_FROM_ALL ${unittest}.cpp $<TARGET_OBJECTS:PYSTON_OBJECTS> $<TARGET_OBJECTS:FROM_CPYTHON>)
  target_link_libraries(${unittest}_unittest stdlib z sqlite3 gmp mpfr ssl crypto readline pypa liblz4 double-conversion unwind gtest gtest_main util ${LLVM_LIBS} ${LIBLZMA_LIBRARIES})
  add_dependencies(unittests ${unittest}_unittest)
endmacro()

add_unittest(gc)
add_unittest(analysis)
add_custom_command(TARGET analysis_unittest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/test/unittests/analysis_listcomp.py ${CMAKE_BINARY_DIR}/test/unittests/analysis_listcomp.py)
add_custom_command(TARGET analysis_unittest POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/test/unittests/analysis_osr.py ${CMAKE_BINARY_DIR}/test/unittests/analysis_osr.py)
