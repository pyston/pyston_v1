cmake_minimum_required(VERSION 2.8)
project(pyston C CXX ASM)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

include(CheckTypeSize)
include(ExternalProject)

set(DEPS_DIR $ENV{HOME}/pyston_deps)

# set build type to release by default
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release." FORCE)
endif()
if(NOT ${CMAKE_BUILD_TYPE} STREQUAL "Release" AND NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  message(FATAL_ERROR "CMAKE_BUILD_TYPE must be set to Release or Debug")
endif()

option(ENABLE_CCACHE "enable caching compiler output" ON)
option(ENABLE_GIL "threading use GIL" ON)
option(ENABLE_GOLD "enable the gold linker" ON)
option(ENABLE_GPERFTOOLS "enable the google performance tools" OFF)
option(ENABLE_GRWL "threading use GRWL" OFF)
option(ENABLE_INTEL_JIT_EVENTS "LLVM support for Intel JIT Events API" OFF)
option(ENABLE_LLVM_DEBUG "LLVM debug symbols" OFF)
option(ENABLE_OPROFILE "enable oprofile support" OFF)
option(ENABLE_SELF_HOST "use pyston to test pyston" OFF)
option(ENABLE_VALGRIND "pyston valgrind support" OFF)

# automatically use ccache if found
if(ENABLE_CCACHE)
  find_program(CCACHE ccache)
  if(CCACHE)
    message(STATUS "found ccache ${CCACHE}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "CCACHE_CPP2=yes ${CCACHE}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE})
  endif()
endif()

# automatically use the gold linker if found
if(ENABLE_GOLD)
  find_program(GOLD_LINKER ld.gold)
  if(GOLD_LINKER)
    message(STATUS "found the gold linker ${GOLD_LINKER}")
    set(CMAKE_LINKER "${GOLD_LINKER}")
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -B${CMAKE_SOURCE_DIR}/tools/build_system")
  endif()
endif()

# pyston self host mode
if(ENABLE_SELF_HOST)
  set(PYTHON_EXE "pyston")
else()
  find_program(PYTHON_EXE python)
endif()

# initial clang flags (set here so they're used when building llvm)
set(CLANG_FLAGS "-Qunused-arguments -fcolor-diagnostics" CACHE STRING "Clang specific C and CXX flags")
if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CLANG_FLAGS}")
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS}")
endif()

# llvm disable debug info unless ENABLE_LLVM_DEBUG is ON
if(${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND NOT ENABLE_LLVM_DEBUG)
  set(CMAKE_CXX_FLAGS_DEBUG "-O3" CACHE STRING "" FORCE)
endif()

execute_process(COMMAND cat llvm_revision.txt WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE LLVMREV OUTPUT_STRIP_TRAILING_WHITESPACE)

# llvm and clang patches
add_custom_target(llvm_gotorev python ${CMAKE_SOURCE_DIR}/tools/git_svn_gotorev.py ${DEPS_DIR}/llvm-trunk ${LLVMREV} llvm_patches WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(clang_gotorev python ${CMAKE_SOURCE_DIR}/tools/git_svn_gotorev.py ${DEPS_DIR}/llvm-trunk/tools/clang ${LLVMREV} clang_patches WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(llvm_up DEPENDS llvm_gotorev clang_gotorev)

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gitmodules
                   COMMAND git submodule update --init WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                   COMMAND cmake -E touch ${CMAKE_BINARY_DIR}/gitmodules
                   DEPENDS ${CMAKE_SOURCE_DIR}/.gitmodules)
add_custom_target(gitsubmodules DEPENDS ${CMAKE_BINARY_DIR}/gitmodules)

# llvm
set(LLVM_TARGETS_TO_BUILD "host" CACHE STRING "LLVM targets")
#set(LLVM_EXTERNAL_CLANG_SOURCE_DIR "${CMAKE_SOURCE_DIR}/clang" CACHE String "Clang directory")
if(ENABLE_INTEL_JIT_EVENTS)
  set(LLVM_USE_INTEL_JITEVENTS "ON" CACHE STRING "Enable building support for the Intel JIT Events API")
  set(INTEL_JIT_EVENTS_LIB "inteljitevents")
  add_definitions(-DENABLE_INTEL_JIT_EVENTS=1)
endif()
add_subdirectory(${DEPS_DIR}/llvm-trunk ${CMAKE_BINARY_DIR}/llvm EXCLUDE_FROM_ALL)
set(CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}/llvm/share/llvm/cmake/")
include(LLVMConfig)
llvm_map_components_to_libnames(LLVM_LIBS core mcjit native bitreader ipo irreader debuginfodwarf instrumentation ${INTEL_JIT_EVENTS_LIB})

# libunwind
if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  set(LIBUNWIND_DEBUG_CFLAGS "CFLAGS=-O0 -g")
  set(LIBUNWIND_DEBUG "--enable-debug")
  set(LIBUNWIND_DEBUG_FRAME "--enable-debug-frame")
endif()
ExternalProject_Add(libunwind
                    PREFIX libunwind
                    SOURCE_DIR ${CMAKE_SOURCE_DIR}/libunwind
                    DEPENDS gitsubmodules
                    UPDATE_COMMAND autoreconf -i
                    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/libunwind/configure ${LIBUNWIND_DEBUG_CFLAGS} --prefix=${CMAKE_BINARY_DIR}/libunwind --enable-shared=0 ${LIBUNWIND_DEBUG} ${LIBUNWIND_DEBUG_FRAME}
                    LOG_UPDATE ON
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON)

# libpypa
add_subdirectory(libpypa)
add_dependencies(pypa gitsubmodules)

# lz4
add_subdirectory(lz4/cmake_unofficial)
add_dependencies(lz4 gitsubmodules)

# valgrind
if(ENABLE_VALGRIND)
  find_package(Valgrind REQUIRED)
  include_directories(${VALGRIND_INCLUDE_DIR})
  message(STATUS "Including valgrind ${VALGRIND_INCLUDE_DIR}")
else()
  add_definitions(-DNVALGRIND)
endif()

if(ENABLE_GRWL)
  add_definitions(-DTHREADING_USE_GIL=0 -DTHREADING_USE_GRWL=1)
else()
  add_definitions(-DTHREADING_USE_GIL=1 -DTHREADING_USE_GRWL=0)
endif()

if(ENABLE_GPERFTOOLS)
  set(OPTIONAL_LIBRARIES ${OPTIONAL_LIBRARIES} profiler)
endif()

if(ENABLE_OPROFILE)
  set(OPTIONAL_LIBRARIES ${OPTIONAL_LIBRARIES} opagent)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wreturn-type -Wno-sign-compare -Wno-unused -Wno-sign-compare -Wno-unused-parameter -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} -std=c++11 -fno-rtti -fexceptions -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -Woverloaded-virtual -Wno-invalid-offsetof -Wcast-qual -Wno-sign-conversion -Wnon-virtual-dtor -Winit-self -Wmissing-include-dirs -Wstrict-overflow=5 -Wpointer-arith -Wtype-limits -Wwrite-strings -Wempty-body -Waggregate-return -Wmissing-field-initializers -Wredundant-decls -Winline -Wint-to-pointer-cast -Wlong-long -Wvla -Wno-attributes")

set(CLANG_FLAGS "${CLANG_FLAGS} -Wimplicit-int -Wstrict-prototypes -Wold-style-definition -Wnested-externs -Wpointer-to-int-cast -Wno-mismatched-tags -Wno-extern-c-compat")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS}")
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -Wno-aggregate-return -Wno-inline -Wno-redundant-decls -Wno-strict-overflow")
endif()

add_definitions(${LLVM_DEFINITIONS})
add_definitions(-DDEFAULT_PYTHON_MAJOR_VERSION=2 -DDEFAULT_PYTHON_MINOR_VERSION=7 -DDEFAULT_PYTHON_MICRO_VERSION=6) # Python 2.7.6
add_definitions(-DLLVMREV=${LLVMREV})

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(from_cpython/Include)

find_package(LibLZMA REQUIRED)
link_directories(${CMAKE_BINARY_DIR}/libunwind/lib)
link_directories(${LLVM_LIBRARY_DIRS})


# generate config.h - must be after LLVM is configured
set(CMAKE_EXTRA_INCLUDE_FILES ${CMAKE_SOURCE_DIR}/src/runtime/types.h)
set(CMAKE_REQUIRED_DEFINITIONS "-DNVALGRIND -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS")
set(CMAKE_REQUIRED_INCLUDES ${DEPS_DIR}/llvm-trunk/include
                            ${CMAKE_BINARY_DIR}/llvm/include
                            ${CMAKE_SOURCE_DIR}/from_cpython/Include
                            ${CMAKE_SOURCE_DIR}/src)
check_type_size("pyston::BoxedDict" SIZEOF_BOXEDDICT LANGUAGE CXX)
configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)

add_subdirectory(lib_pyston)
add_subdirectory(from_cpython)
add_subdirectory(src)
add_subdirectory(test/test_extension)
add_subdirectory(test/unittests)
add_subdirectory(tools)

add_executable(pyston $<TARGET_OBJECTS:PYSTON_MAIN_OBJECT> $<TARGET_OBJECTS:PYSTON_OBJECTS> $<TARGET_OBJECTS:FROM_CPYTHON>)
# Wrap the stdlib in --whole-archive to force all the symbols to be included and eventually exported
target_link_libraries(pyston -Wl,--whole-archive stdlib -Wl,--no-whole-archive pthread m readline sqlite3 gmp ssl crypto unwind pypa liblz4 double-conversion ${LLVM_LIBS} ${LIBLZMA_LIBRARIES} ${OPTIONAL_LIBRARIES})

# copy src/codegen/parse_ast.py to the build directory
add_custom_command(TARGET pyston POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/codegen/parse_ast.py ${CMAKE_BINARY_DIR}/src/codegen/parse_ast.py)

add_custom_target(astcompare COMMAND ${CMAKE_SOURCE_DIR}/tools/astprint_test.sh
                  DEPENDS astprint
                  COMMENT "Running libpypa vs CPython AST result comparison test")

# test
enable_testing()
set(TEST_THREADS 1 CACHE STRING "number of pyston test threads")
set(PYTHONIOENCODING utf-8)

add_test(NAME lint COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/lint.py WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_test(NAME check-format COMMAND ${CMAKE_SOURCE_DIR}/tools/check_format.sh ${LLVM_TOOLS_BINARY_DIR}/clang-format WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_test(NAME gc_unittest COMMAND gc_unittest)
add_test(NAME analysis_unittest COMMAND analysis_unittest)
add_test(NAME pyston_defaults COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/tester.py -R ./pyston -j${TEST_THREADS} -a=-S -k ${CMAKE_SOURCE_DIR}/test/tests)
# we pass -I to cpython tests and skip failing ones b/c they are slooow otherwise
add_test(NAME pyston_defaults_cpython_tests COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/tester.py -R ./pyston -j${TEST_THREADS} -a=-S -k --exit-code-only --skip-failing ${CMAKE_SOURCE_DIR}/test/cpython)
add_test(NAME pyston_defaults_integration_tests COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/tester.py -R ./pyston -j${TEST_THREADS} -a=-S -k --exit-code-only --skip-failing -t60 ${CMAKE_SOURCE_DIR}/test/integration)
add_test(NAME pyston_max_compilation_tier COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/tester.py -R ./pyston -j${TEST_THREADS} -a=-O -a=-S -k ${CMAKE_SOURCE_DIR}/test/tests)
add_test(NAME pyston_old_parser COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/tester.py -a=-x -R ./pyston -j1 -a=-n -a=-S -k ${CMAKE_SOURCE_DIR}/test/tests)

# format
file(GLOB_RECURSE FORMAT_FILES ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_custom_target(format ${LLVM_TOOLS_BINARY_DIR}/clang-format -style=file -i ${FORMAT_FILES} DEPENDS clang-format WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_custom_target(check-format ${CMAKE_SOURCE_DIR}/tools/check_format.sh ${LLVM_TOOLS_BINARY_DIR}/clang-format DEPENDS clang-format WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)

# lint
add_custom_target(lint ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/lint.py WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)

# check
add_custom_target(check-pyston COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS pyston copy_stdlib copy_libpyston clang-format ext_cpython ext_pyston unittests) 

# {run,dbg,perf,memcheck,memleaks,cachegrind}_TESTNAME
file(GLOB RUNTARGETS ${CMAKE_SOURCE_DIR}/test/tests/*.py ${CMAKE_SOURCE_DIR}/microbenchmarks/*.py ${CMAKE_SOURCE_DIR}/minibenchmarks/*.py)
foreach(RUNTARGET ${RUNTARGETS})
  get_filename_component(BASEFILENAME ${RUNTARGET} NAME_WE)
  add_custom_target(run_${BASEFILENAME} ./pyston -q ${RUNTARGET} DEPENDS pyston)
  add_custom_target(dbg_${BASEFILENAME} gdb --ex "set confirm off" --ex "handle SIGUSR2 pass nostop noprint" --ex run --ex "bt 20" --args ./pyston -q ${RUNTARGET} DEPENDS pyston)
  add_custom_target(perf_${BASEFILENAME} perf record -g -- ./pyston -q -p ${RUNTARGET}
                    COMMAND perf report -v -n -g flat,1000 | bash ${CMAKE_SOURCE_DIR}/tools/cumulate.sh | less -S)

  if(ENABLE_VALGRIND)
    add_custom_target(memcheck_${BASEFILENAME} valgrind --tool=memcheck --leak-check=no --db-attach=yes ./pyston ${RUNTARGET} DEPENDS pyston)
    add_custom_target(memleaks_${BASEFILENAME} valgrind --tool=memcheck --leak-check=full --leak-resolution=low --show-reachable=yes ./pyston ${RUNTARGET} DEPENDS pyston)
    add_custom_target(cachegrind_${BASEFILENAME} valgrind --tool=cachegrind ./pyston ${RUNTARGET} DEPENDS pyston)
  endif()
endforeach()

# doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
  configure_file(${CMAKE_SOURCE_DIR}/docs/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
  add_custom_target(docs ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
else()
  add_custom_target(docs COMMAND ${CMAKE_COMMAND} -E echo "Can't create docs, doxygen not installed \(try sudo apt-get install doxygen grpahviz on Ubuntu and then rerun cmake\)" VERBATIM)
endif()
