set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_FLAGS_DEBUG "-g -DBINARY_SUFFIX= -DBINARY_STRIPPED_SUFFIX=_stripped")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fstrict-aliasing -enable-tbaa -DNVALGRIND -DBINARY_SUFFIX=_release -DBINARY_STRIPPED_SUFFIX=")

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GITREV OUTPUT_STRIP_TRAILING_WHITESPACE)
set_source_files_properties(jit.cpp PROPERTIES COMPILE_DEFINITIONS "GITREV=${GITREV}")

include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_BINARY_DIR}/libunwind/include)
include_directories(${CMAKE_SOURCE_DIR}/libpypa/src)

file(GLOB ANALYSIS_SRCS analysis/*.cpp)
file(GLOB ASM_WRITING_SRCS asm_writing/*.cpp)
file(GLOB CAPI_SRCS capi/*.c capi/*.cpp)
file(GLOB CODEGEN_IRGEN_SRCS codegen/irgen/*.cpp)
file(GLOB CODEGEN_OPT_SRCS codegen/opt/*.cpp)
file(GLOB CODEGEN_SRCS codegen/*.cpp)
file(GLOB CORE_SRCS core/*.cpp)
file(GLOB GC_SRCS gc/*.cpp)
file(GLOB RUNTIME_BUILTIN_MODULES_SRCS runtime/builtin_modules/*.cpp)
file(GLOB RUNTIME_SRCS runtime/*.cpp runtime/*.S)

if(ENABLE_GPERFTOOLS)
  set(OPTIONAL_SRCS ${OPTIONAL_SRCS} codegen/profiling/pprof.cpp)
endif()

if(ENABLE_OPROFILE)
  set(OPTIONAL_SRCS codegen/profiling/oprofile.cpp)
endif()

add_library(PYSTON_OBJECTS OBJECT codegen/profiling/profiling.cpp ${ANALYSIS_SRCS} ${ASM_WRITING_SRCS} ${CAPI_SRCS} ${CODEGEN_SRCS} ${CODEGEN_IRGEN_SRCS} ${CODEGEN_OPT_SRCS} ${CORE_SRCS} ${GC_SRCS} ${RUNTIME_BUILTIN_MODULES_SRCS} ${RUNTIME_SRCS} ${OPTIONAL_SRCS})
add_dependencies(PYSTON_OBJECTS libunwind pypa ${LLVM_LIBS})

add_library(PYSTON_MAIN_OBJECT OBJECT jit.cpp)
add_dependencies(PYSTON_MAIN_OBJECT libunwind pypa ${LLVM_LIBS})

# build stdlib
add_subdirectory(runtime/inline)

add_custom_target(cpplint python ${CMAKE_SOURCE_DIR}/tools/cpplint.py --filter=-whitespace,-build/header_guard,-build/include_order,-readability/todo ${ANALYSIS_SRCS} ${ASM_WRITING_SRCS} ${CAPI_SRCS} ${CODEGEN_SRCS} ${CODEGEN_IRGEN_SRCS} ${CODEGEN_OPT_SRCS} ${CORE_SRCS} ${GC_SRCS} ${RUNTIME_SRCS} ${RUNTIME_BUILTIN_MODULES_SRCS} ${FROM_CPYTHON_SRCS} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
