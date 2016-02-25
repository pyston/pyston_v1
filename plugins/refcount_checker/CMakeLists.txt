# from llvm-trunk/tools/clang/CMakeLists.txt:
set(LLVM_RUNTIME_OUTPUT_INTDIR llvm/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR llvm/lib${LLVM_LIBDIR_SUFFIX})

# Pyston addition:
include_directories("${DEPS_DIR}/llvm-trunk/tools/clang/include")
include_directories("${CMAKE_BINARY_DIR}/llvm/tools/clang/include")

# From llvm-trunk/tools/clang/tools/clang-check/CMakeLists.txt
set(LLVM_LINK_COMPONENTS
  Option
  Support
  )

add_clang_executable(refcount_checker
  refcount_checker.cpp
  )

target_link_libraries(refcount_checker
  clangAST
  clangBasic
  clangDriver
  clangFrontend
  clangRewriteFrontend
  clangStaticAnalyzerFrontend
  clangTooling
  )
