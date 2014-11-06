add_executable(publicize publicize.cpp)
add_executable(demangle EXCLUDE_FROM_ALL demangle.cpp)
add_executable(mcjitcache EXCLUDE_FROM_ALL mcjitcache.cpp)

llvm_map_components_to_libnames(LLVM_LIBS_EXTRA BitWriter)

target_link_libraries(publicize curses dl pthread ${LLVM_LIBS} ${LLVM_LIBS_EXTRA}) 
add_dependencies(publicize ${LLVM_LIBS_EXTRA})
