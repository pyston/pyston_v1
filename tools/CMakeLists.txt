add_executable(demangle EXCLUDE_FROM_ALL demangle.cpp)
add_executable(mcjitcache EXCLUDE_FROM_ALL mcjitcache.cpp)

llvm_map_components_to_libnames(LLVM_LIBS_EXTRA BitWriter)

add_executable(publicize publicize.cpp)
target_link_libraries(publicize curses dl pthread ${LLVM_LIBS} ${LLVM_LIBS_EXTRA}) 

include_directories(${CMAKE_SOURCE_DIR}/src)
add_executable(astprint EXCLUDE_FROM_ALL astprint.cpp $<TARGET_OBJECTS:PYSTON_OBJECTS> $<TARGET_OBJECTS:FROM_CPYTHON>)
target_link_libraries(astprint stdlib gmp ssl crypto readline pypa double-conversion unwind ${LLVM_LIBS} ${LIBLZMA_LIBRARIES})
  
