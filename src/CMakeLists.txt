set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_FLAGS_DEBUG "-g -DBINARY_SUFFIX= -DBINARY_STRIPPED_SUFFIX=_stripped")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fstrict-aliasing -enable-tbaa -DNVALGRIND -DBINARY_SUFFIX=_release -DBINARY_STRIPPED_SUFFIX=")

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GITREV OUTPUT_STRIP_TRAILING_WHITESPACE)
set_source_files_properties(jit.cpp PROPERTIES COMPILE_DEFINITIONS "GITREV=${GITREV}")

include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_BINARY_DIR}/libunwind/include)
include_directories(${CMAKE_SOURCE_DIR}/libpypa/src)
include_directories(${CMAKE_SOURCE_DIR}/lz4/lib)

if(ENABLE_GPERFTOOLS)
  set(OPTIONAL_SRCS ${OPTIONAL_SRCS} codegen/profiling/pprof.cpp)
endif()

if(ENABLE_OPROFILE)
  set(OPTIONAL_SRCS codegen/profiling/oprofile.cpp)
endif()

add_library(PYSTON_OBJECTS OBJECT ${OPTIONAL_SRCS}
		analysis/function_analysis.cpp
		analysis/scoping_analysis.cpp
		analysis/type_analysis.cpp
		asm_writing/assembler.cpp
		asm_writing/disassemble.cpp
		asm_writing/icinfo.cpp
		asm_writing/mc_writer.cpp
		asm_writing/rewriter.cpp
		capi/abstract.cpp
		capi/codecs.cpp
		capi/errors.cpp
		capi/modsupport.cpp
		capi/object.cpp
		capi/typeobject.cpp
		codegen/ast_interpreter.cpp
		codegen/ast_interpreter_exec.S
		codegen/baseline_jit.cpp
		codegen/codegen.cpp
		codegen/compvars.cpp
		codegen/entry.cpp
		codegen/gcbuilder.cpp
		codegen/irgen.cpp
		codegen/irgen/future.cpp
		codegen/irgen/hooks.cpp
		codegen/irgen/irgenerator.cpp
		codegen/irgen/util.cpp
		codegen/memmgr.cpp
		codegen/opt/aa.cpp
		codegen/opt/boxing_passes.cpp
		codegen/opt/const_classes.cpp
		codegen/opt/dead_allocs.cpp
		codegen/opt/escape_analysis.cpp
		codegen/opt/inliner.cpp
		codegen/opt/mallocs_nonnull.cpp
		codegen/opt/util.cpp
		codegen/parser.cpp
		codegen/patchpoints.cpp
		codegen/profiling/dumprof.cpp
		codegen/profiling/profiling.cpp
		codegen/pypa-parser.cpp
		codegen/runtime_hooks.cpp
		codegen/serialize_ast.cpp
		codegen/stackmaps.cpp
		codegen/type_recording.cpp
		codegen/unwinding.cpp
		core/ast.cpp
		core/cfg.cpp
		core/options.cpp
		core/stats.cpp
		core/stringpool.cpp
		core/threading.cpp
		core/util.cpp
		deadlock_debug_helper.cpp
		gc/collector.cpp
		gc/gc_alloc.cpp
		gc/heap.cpp
		runtime/bool.cpp
		runtime/builtin_modules/ast.cpp
		runtime/builtin_modules/builtins.cpp
		runtime/builtin_modules/gc.cpp
		runtime/builtin_modules/pyston.cpp
		runtime/builtin_modules/sys.cpp
		runtime/builtin_modules/thread.cpp
		runtime/capi.cpp
		runtime/classobj.cpp
		runtime/code.cpp
		runtime/complex.cpp
		runtime/ctxswitching.S
		runtime/cxx_unwind.cpp
		runtime/descr.cpp
		runtime/dict.cpp
		runtime/file.cpp
		runtime/float.cpp
		runtime/frame.cpp
		runtime/generator.cpp
		runtime/ics.cpp
		runtime/import.cpp
		runtime/int.cpp
		runtime/iterators.cpp
		runtime/iterobject.cpp
		runtime/list.cpp
		runtime/long.cpp
		runtime/objmodel.cpp
		runtime/set.cpp
		runtime/stacktrace.cpp
		runtime/str.cpp
		runtime/super.cpp
		runtime/traceback.cpp
		runtime/tuple.cpp
		runtime/types.cpp
		runtime/util.cpp
		)

add_dependencies(PYSTON_OBJECTS copy_stdlib libunwind_patched libunwind pypa ${LLVM_LIBS})

add_library(PYSTON_MAIN_OBJECT OBJECT jit.cpp)
add_dependencies(PYSTON_MAIN_OBJECT copy_stdlib libunwind_patched libunwind pypa liblz4 ${LLVM_LIBS})

# build stdlib
add_subdirectory(runtime/inline)

add_custom_target(cpplint python ${CMAKE_SOURCE_DIR}/tools/cpplint.py --filter=-whitespace,-build/header_guard,-build/include_order,-readability/todo ${ANALYSIS_SRCS} ${ASM_WRITING_SRCS} ${CAPI_SRCS} ${CODEGEN_SRCS} ${CODEGEN_IRGEN_SRCS} ${CODEGEN_OPT_SRCS} ${CORE_SRCS} ${GC_SRCS} ${RUNTIME_SRCS} ${RUNTIME_BUILTIN_MODULES_SRCS} ${FROM_CPYTHON_SRCS} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
