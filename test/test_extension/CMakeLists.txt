set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers")
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".pyston${CMAKE_SHARED_LIBRARY_SUFFIX}")

add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/build/lib.linux-x86_64-2.7/basic_test.so
                   COMMAND python setup.py build
                   DEPENDS basic_test.c descr_test.c slots_test.c
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_library(basic_test SHARED basic_test.c)
add_library(descr_test SHARED descr_test.c)
add_library(slots_test SHARED slots_test.c)

add_custom_target(ext_cpython DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/build/lib.linux-x86_64-2.7/basic_test.so)
add_custom_target(ext_pyston DEPENDS basic_test descr_test slots_test)
