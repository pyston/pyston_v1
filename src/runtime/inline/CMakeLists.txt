# Copyright (c) 2010-2011, Rob Jansen
# 
# To the extent that a federal employee is an author of a portion of
# this software or a derivative work thereof, no copyright is claimed by
# the United States Government, as represented by the Secretary of the
# Navy ("GOVERNMENT") under Title 17, U.S. Code. All Other Rights 
# Reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the names of the copyright owners nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# GOVERNMENT ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS" CONDITION
# AND DISCLAIMS ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
# RESULTING FROM THE USE OF THIS SOFTWARE.

# adapted from https://github.com/shadow/shadow/blob/master/cmake/LLVMTools.cmake

# don't do pgo here, as our gcc command line flags are incompatible with clang, and we
# don't want to bake the instrumentation into the resulting bitcode anyway.
REMOVE_PROFILE_FLAGS()

set(BC_INLINE_SRCS boxing.cpp dict.cpp link_forcer.cpp list.cpp tuple.cpp xrange.cpp)

include_directories(${CMAKE_SOURCE_DIR}/src)

file(GLOB_RECURSE PYSTON_HEADERS ${CMAKE_SOURCE_DIR}/src/*.h)
file(GLOB_RECURSE FROM_CPYTHON_HEADERS ${CMAKE_SOURCE_DIR}/from_cpython/*.h)

# set compiler
if(CCACHE)
  set(BC_CXX ${CCACHE} ${LLVM_TOOLS_BINARY_DIR}/clang++)
else()
  set(BC_CXX ${LLVM_TOOLS_BINARY_DIR}/clang++)
endif()

# set CXX flags
set(BC_CXX_FLAGS "")
if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  separate_arguments(BC_CXX_FLAGS UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
else()
  separate_arguments(BC_CXX_FLAGS UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
  set(OPT_OPTIONS "-strip-debug")
  set(STDLIB_SUFFIX ".release")
endif()

# set defines
set(BC_DEFINES "")
get_directory_property(COMPILE_DEFINITIONS COMPILE_DEFINITIONS)
foreach(DEFINITION ${COMPILE_DEFINITIONS})
  list(APPEND BC_DEFINES -D${DEFINITION})
endforeach(DEFINITION)

# set includes
set(BC_INCLUDES "")
get_directory_property(INCLUDE_DIRECTORIES INCLUDE_DIRECTORIES)
foreach(DIRECTORY ${INCLUDE_DIRECTORIES})
  list(APPEND BC_INCLUDES -I${DIRECTORY})
endforeach(DIRECTORY)

set(BC_FILES "")
set(BC_OBJS "")
foreach(BC_SRC ${BC_INLINE_SRCS})
  get_filename_component(BC_SRC_BASENAME ${BC_SRC} NAME)
  get_filename_component(BC_SRC_FULLPATH ${BC_SRC} ABSOLUTE)

  # generate the bitcode for this file
  add_custom_command(OUTPUT ${BC_SRC_BASENAME}.bc
                     COMMAND ${BC_CXX} ${BC_DEFINES} ${BC_CXX_FLAGS} ${BC_INCLUDES} -c ${BC_SRC_FULLPATH} -o ${BC_SRC_BASENAME}.bc -emit-llvm
                     DEPENDS ${BC_SRC_FULLPATH} clang ${PYSTON_HEADERS} ${FROM_CPYTHON_HEADERS}
                     COMMENT "Building LLVM bitcode ${BC_SRC_BASENAME}.bc"
                     VERBATIM)
  set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${BC_SRC_BASENAME}.bc)

  # publicize the bitcode file
  add_custom_command(OUTPUT ${BC_SRC_BASENAME}.pub.bc
                     COMMAND ${CMAKE_BINARY_DIR}/tools/publicize ${BC_SRC_BASENAME}.bc -o ${BC_SRC_BASENAME}.pub.bc
                     DEPENDS ${BC_SRC_BASENAME}.bc publicize
                     COMMENT "Publicizing LLVM bitcode ${BC_SRC_BASENAME}.pub.bc"
                     VERBATIM)
  set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${BC_SRC_BASENAME}.pub.bc)
  set_source_files_properties(${BC_SRC_BASENAME}.pub.bc PROPERTIES LANGUAGE CXX)

  # compile into a regular object file
  add_custom_command(OUTPUT ${BC_SRC_BASENAME}.o
                     COMMAND ${BC_CXX} ${BC_DEFINES} ${BC_CXX_FLAGS} ${BC_INCLUDES} -c ${BC_SRC_BASENAME}.pub.bc -o ${BC_SRC_BASENAME}.o
                     DEPENDS ${BC_SRC_BASENAME}.pub.bc clang
                     COMMENT "Compiling ${BC_SRC_BASENAME}.pub.bc -> ${BC_SRC_BASENAME}.o"
                     VERBATIM)
  set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${BC_SRC_BASENAME}.o)
  set_source_files_properties(${BC_SRC_BASENAME}.o PROPERTIES EXTERNAL_OBJECT true GENERATED true)

  # keep track of every bitcode and object file we created
  list(APPEND BC_FILES ${BC_SRC_BASENAME}.pub.bc)
  list(APPEND BC_OBJS ${BC_SRC_BASENAME}.o)
endforeach(BC_SRC)

# link all the bitcode files together
add_custom_command(OUTPUT stdlib.unopt.bc
                   COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link -o ${CMAKE_CURRENT_BINARY_DIR}/stdlib.unopt.bc ${BC_FILES}
                   DEPENDS ${BC_FILES} llvm-link
                   COMMENT "Linking stdlib.unopt.bc")

# optimize stdlib
add_custom_command(OUTPUT stdlib${STDLIB_SUFFIX}.bc
                   COMMAND ${LLVM_TOOLS_BINARY_DIR}/opt -O3 ${OPT_OPTIONS} stdlib.unopt.bc -o stdlib${STDLIB_SUFFIX}.bc
                   DEPENDS stdlib.unopt.bc opt
                   COMMENT "Optimizing stdlib.unopt.bc -> stdlib.bc")

add_custom_command(OUTPUT stdlib.bc.o
                   COMMAND ${CMAKE_LINKER} -r -b binary stdlib${STDLIB_SUFFIX}.bc -o ${CMAKE_CURRENT_BINARY_DIR}/stdlib.bc.o
                   DEPENDS stdlib${STDLIB_SUFFIX}.bc
                   COMMENT "Embedding stdlib${STDLIB_SUFFIX}.bc -> stdlib.bc.o")

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  add_custom_command(OUTPUT stdlib.stripped.bc
                     COMMAND ${LLVM_TOOLS_BINARY_DIR}/opt -strip-debug stdlib.bc -o ${CMAKE_CURRENT_BINARY_DIR}/stdlib.stripped.bc
                     DEPENDS stdlib.bc opt
                     COMMENT "Stripping stdlib.bc -> stdlib.stripped.bc")

  add_custom_command(OUTPUT stdlib.stripped.bc.o
                     COMMAND ${CMAKE_LINKER} -r -b binary stdlib.stripped.bc -o ${CMAKE_CURRENT_BINARY_DIR}/stdlib.stripped.bc.o
                     DEPENDS stdlib.stripped.bc
                     COMMENT "Embedding stdlib.stripped.bc -> stdlib.stripped.bc.o")
  add_library(stdlib stdlib.stripped.bc.o stdlib.bc.o ${BC_OBJS})
else()
  add_library(stdlib stdlib.bc.o ${BC_OBJS})
endif()
set_property(TARGET stdlib PROPERTY LINKER_LANGUAGE C)
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "stdlib.unopt.bc;stdlib.bc;stdlib.bc.o;stdlib.stripped.bc;stdlib.stripped.bc.o;stdlib.release.bc")

