add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build/lib.linux-x86_64-3.5/basic_test.so
                   COMMAND python3 setup.py build --build-lib ${CMAKE_CURRENT_BINARY_DIR}/build/lib.linux-x86_64-3.5
                   DEPENDS basic_test.c descr_test.c slots_test.c type_test.c api_test.c
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/basic_test.pyston.so
                   COMMAND ${CMAKE_BINARY_DIR}/pyston setup.py build --build-lib ${CMAKE_CURRENT_BINARY_DIR}
                   DEPENDS pyston copy_stdlib basic_test.c descr_test.c slots_test.c type_test.c api_test.c
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# add_custom_target(ext_cpython DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/build/lib.linux-x86_64-3.5/basic_test.so)
add_custom_target(ext_cpython DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/basic_test.pyston.so)
add_custom_target(ext_pyston DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/basic_test.pyston.so)
