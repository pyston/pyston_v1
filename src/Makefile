# Disable builtin rules:
.SUFFIXES:

USE_TEST_LLVM := 0
DEPS_DIR := $(HOME)/pyston_deps

LLVM_REVISION_FILE := ../llvm_revision.txt
LLVM_REVISION := $(shell cat $(LLVM_REVISION_FILE))

USE_CLANG  := 1
USE_CCACHE := 1
USE_DISTCC := 0

ENABLE_VALGRIND := 0

GDB := gdb
GCC_DIR := $(DEPS_DIR)/gcc-4.8.2-install
GTEST_DIR := $(DEPS_DIR)/gtest-1.7.0

USE_DEBUG_LIBUNWIND := 0

PYTHON_MAJOR_VERSION := 2
PYTHON_MINOR_VERSION := 7
PYTHON_MICRO_VERSION := 3

MAX_MEM_KB := 500000
MAX_DBG_MEM_KB := 500000

TEST_THREADS := 4

ERROR_LIMIT := 10
COLOR := 1

VERBOSE := 0

ENABLE_INTEL_JIT_EVENTS := 0

# Setting this to 1 will set the Makefile to use binaries from the trunk
# directory, even if USE_TEST_LLVM is set to 1.
# This is useful if clang isn't installed into the test directory, ex due
# to disk space concerns.
FORCE_TRUNK_BINARIES := 0

# Put any overrides in here:
-include Makefile.local


TOOLS_DIR := ../tools
TESTS_DIR := ../test/tests

GPP := $(GCC_DIR)/bin/g++

ifeq ($(V),1)
	VERBOSE := 1
endif
ifeq ($(VERBOSE),1)
	VERB :=
	ECHO := @\#
else
	VERB := @
	ECHO := @ echo pyston:
endif

LLVM_TRUNK_SRC := $(DEPS_DIR)/llvm-trunk
LLVM_TEST_SRC := $(DEPS_DIR)/llvm-test
LLVM_TRUNK_BUILD := $(DEPS_DIR)/llvm-trunk-build
LLVM_TEST_BUILD := $(DEPS_DIR)/llvm-test-build
ifneq ($(USE_TEST_LLVM),0)
	LLVM_SRC := $(LLVM_TEST_SRC)
	LLVM_BUILD := $(LLVM_TEST_BUILD)
else
	LLVM_SRC := $(LLVM_TRUNK_SRC)
	LLVM_BUILD := $(LLVM_TRUNK_BUILD)
endif

ifeq ($(FORCE_TRUNK_BINARIES),1)
	LLVM_BIN := $(LLVM_TRUNK_BUILD)/Release/bin
else
	LLVM_BIN := $(LLVM_BUILD)/Release/bin
endif

LLVM_LINK_LIBS := core mcjit native bitreader ipo irreader jit debuginfo instrumentation
ifneq ($(ENABLE_INTEL_JIT_EVENTS),0)
LLVM_LINK_LIBS += inteljitevents
endif

LLVM_CXXFLAGS := $(shell $(LLVM_BUILD)/Release+Asserts/bin/llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_BUILD)/Release+Asserts/bin/llvm-config --ldflags --libs $(LLVM_LINK_LIBS))
LLVM_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Release+Asserts/lib/*)

LLVM_DEBUG_LDFLAGS := $(shell $(LLVM_BUILD)/Debug+Asserts/bin/llvm-config --ldflags --libs $(LLVM_LINK_LIBS))
LLVM_DEBUG_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Debug+Asserts/lib/*)

LLVM_CONFIG_RELEASE := $(LLVM_BUILD)/Release/bin/llvm-config
ifneq ($(wildcard $(LLVM_CONFIG_RELEASE)),)
LLVM_RELEASE_CXXFLAGS := $(shell $(LLVM_BUILD)/Release/bin/llvm-config --cxxflags)
LLVM_RELEASE_LDFLAGS := $(shell $(LLVM_BUILD)/Release/bin/llvm-config --ldflags --libs $(LLVM_LINK_LIBS))
LLVM_RELEASE_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Release/lib/*)
else
LLVM_RELEASE_CXXFLAGS := RELEASE_NOT_BUILT
LLVM_RELEASE_LDFLAGS := RELEASE_NOT_BUILT
LLVM_RELEASE_LIB_DEPS := RELEASE_NOT_BUILT
endif

ifneq ($(wildcard /usr/local/include/llvm),)
# Global include files can screw up the build, since if llvm moves a header file,
# the compiler will silently fall back to the global one that still exists.
# These include files are persistent because llvm's "make uninstall" does *not*
# delete them if the uninstall command is run on a revision that didn't include
# those files.
# This could probably be handled (somehow blacklist this particular folder?),
# but for now just error out:
$(error "Error: global llvm include files detected")
endif

# Note: use lazy-expansion for these profile targets, since calling the profile llvm-config will
# actually generate a gmon.out file!
LLVM_PROFILE_CXXFLAGS = $(shell $(LLVM_BUILD)/Release+Profile/bin/llvm-config --cxxflags) -UNDEBUG
LLVM_PROFILE_LDFLAGS = $(shell $(LLVM_BUILD)/Release+Profile/bin/llvm-config --ldflags --libs $(LLVM_LINK_LIBS))
LLVM_PROFILE_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Release+Profile/lib/*)

CLANG_EXE := $(LLVM_BIN)/clang++

COMMON_CXXFLAGS := -g -Werror -Wreturn-type -Woverloaded-virtual -Wall -Wno-sign-compare -Wno-unused -I. -I../include -fno-omit-frame-pointer
COMMON_CXXFLAGS += -std=c++11
COMMON_CXXFLAGS += -Wextra -Wno-sign-compare
COMMON_CXXFLAGS += -Wno-unused-parameter # should use the "unused" attribute
COMMON_CXXFLAGS += -fexceptions -fno-rtti
COMMON_CXXFLAGS += -Wno-invalid-offsetof # allow the use of "offsetof", and we'll just have to make sure to only use it legally.
COMMON_CXXFLAGS += -DENABLE_INTEL_JIT_EVENTS=$(ENABLE_INTEL_JIT_EVENTS)

ifeq ($(ENABLE_VALGRIND),0)
	COMMON_CXXFLAGS += -DNVALGRIND
	VALGRIND := false
else
	COMMON_CXXFLAGS += -I$(DEPS_DIR)/valgrind-3.9.0/include
	VALGRIND := VALGRIND_LIB=$(DEPS_DIR)/valgrind-3.9.0-install/lib/valgrind $(DEPS_DIR)/valgrind-3.9.0-install/bin/valgrind
endif

COMMON_CXXFLAGS += -DGITREV=$(shell git rev-parse HEAD | head -c 12) -DLLVMREV=$(LLVM_REVISION)
COMMON_CXXFLAGS += -DDEFAULT_PYTHON_MAJOR_VERSION=$(PYTHON_MAJOR_VERSION) -DDEFAULT_PYTHON_MINOR_VERSION=$(PYTHON_MINOR_VERSION) -DDEFAULT_PYTHON_MICRO_VERSION=$(PYTHON_MICRO_VERSION)

# Use our "custom linker" that calls gold if available
COMMON_LDFLAGS := -B../tools/build_system -L/usr/local/lib -lpthread -ldl -lcurses -lm -lunwind -lz -llzma -L$(DEPS_DIR)/gcc-4.8.2-install/lib64 -lreadline
# Make sure that we put all symbols in the dynamic symbol table so that MCJIT can load them;
# TODO should probably do the linking before MCJIT
COMMON_LDFLAGS += -Wl,-E

# The system libstdc++ will likely be too old for Pyston, but we made a brand new one
# as part of our GCC installation.  There are a couple ways to use that one instead:
# 1) Install the built libstdc++ (~/pyston_deps/gcc-4.8.2-install/lib64/libstdc++.so.6.0.18) and update the global shared library (/usr/local/x86_64-linux-gnu/libstdc++.so.6) to point to it
# 2) Set LD_LIBRARY_PATH=~/pyston_deps/gcc-4.8.2-install/lib64
# 3) Link it in statically.
#
# For now, go with option #3 since it is the only one that requires no extra work for people who want to run Pyston.
#
# (I was surprised by this issue: the linker sees the shared library that we built
# as part of GCC and decides to dynamically link to that, even though at load time we'll
# end up linking to the system libstdc++ instead.)
COMMON_LDFLAGS += -static-libstdc++

ifneq ($(USE_DEBUG_LIBUNWIND),0)
	COMMON_LDFLAGS += -L$(DEPS_DIR)/libunwind-trunk-debug-install/lib

	# libunwind's include files warn on -Wextern-c-compat, so turn that off;
	# ideally would just turn it off for header files in libunwind, maybe by
	# having an internal libunwind.h that pushed/popped the diagnostic state,
	# but it doesn't seem like that important a warning so just turn it off.
	COMMON_CXXFLAGS += -I$(DEPS_DIR)/libunwind-trunk-debug-install/include -Wno-extern-c-compat
else
	COMMON_LDFLAGS += -L$(DEPS_DIR)/libunwind-trunk-install/lib
	COMMON_CXXFLAGS += -I$(DEPS_DIR)/libunwind-trunk-install/include -Wno-extern-c-compat
endif


EXTRA_CXXFLAGS ?=
CXXFLAGS_DBG := $(LLVM_CXXFLAGS) $(COMMON_CXXFLAGS) -O0 -DBINARY_SUFFIX= -DBINARY_STRIPPED_SUFFIX=_stripped $(EXTRA_CXXFLAGS)
CXXFLAGS_PROFILE = $(LLVM_PROFILE_CXXFLAGS) $(COMMON_CXXFLAGS) -pg -O3 -DNDEBUG -DNVALGRIND -DBINARY_SUFFIX=_release -DBINARY_STRIPPED_SUFFIX= -fno-function-sections $(EXTRA_CXXFLAGS)
CXXFLAGS_RELEASE := $(LLVM_RELEASE_CXXFLAGS) $(COMMON_CXXFLAGS) -O3 -fstrict-aliasing -enable-tbaa -DNDEBUG -DNVALGRIND -DBINARY_SUFFIX=_release -DBINARY_STRIPPED_SUFFIX= $(EXTRA_CXXFLAGS)

LDFLAGS := $(LLVM_LDFLAGS) $(COMMON_LDFLAGS)
LDFLAGS_DEBUG := $(LLVM_DEBUG_LDFLAGS) $(COMMON_LDFLAGS)
LDFLAGS_PROFILE = $(LLVM_PROFILE_LDFLAGS) -pg $(COMMON_LDFLAGS)
LDFLAGS_RELEASE := $(LLVM_RELEASE_LDFLAGS) $(COMMON_LDFLAGS)
LDFLAGS_RELEASE := $(LLVM_RELEASE_LDFLAGS) $(COMMON_LDFLAGS)
# Can't add this, because there are functions in the compiler that look unused but are hooked back from the runtime:
# LDFLAGS_RELEASE += -Wl,--gc-sections


BUILD_SYSTEM_DEPS := Makefile Makefile.local $(wildcard build_system/*)
CLANG_DEPS := $(CLANG_EXE) $(abspath $(dir $(CLANG_EXE))/../../built_release)

# settings to make clang and ccache play nicely:
CLANG_CCACHE_FLAGS := -Qunused-arguments
CLANG_EXTRA_FLAGS := -Wno-mismatched-tags -ferror-limit=$(ERROR_LIMIT) $(CLANG_CCACHE_FLAGS)
ifeq ($(COLOR),1)
	CLANG_EXTRA_FLAGS += -fcolor-diagnostics
else
	CLANG_EXTRA_FLAGS += -fno-color-diagnostics
endif
CLANGFLAGS := $(CXXFLAGS_DBG) $(CLANG_EXTRA_FLAGS)
CLANGFLAGS_RELEASE := $(CXXFLAGS_RELEASE) $(CLANG_EXTRA_FLAGS)

# Extra flags to enable soon:
CLANGFLAGS += -Wno-sign-conversion -Wnon-virtual-dtor -Winit-self -Wimplicit-int -Wmissing-include-dirs -Wstrict-overflow=5 -Wundef -Wpointer-arith -Wtype-limits -Wwrite-strings -Wempty-body -Waggregate-return -Wstrict-prototypes -Wold-style-definition -Wmissing-field-initializers -Wredundant-decls -Wnested-externs -Winline -Wint-to-pointer-cast -Wpointer-to-int-cast -Wlong-long -Wvla
# Want this one but there's a lot of places that I haven't followed it:
# CLANGFLAGS += -Wold-style-cast
# llvm headers fail on this one:
# CLANGFLAGS += -Wswitch-enum
# Not sure about these:
# CLANGFLAGS += -Wbad-function-cast -Wcast-qual -Wcast-align -Wmissing-prototypes -Wunreachable-code -Wfloat-equal -Wunused -Wunused-variable
# Or everything:
# CLANGFLAGS += -Weverything -Wno-c++98-compat-pedantic -Wno-shadow -Wno-padded -Wno-zero-length-array

CXX := $(GPP)
CXX_PROFILE := $(GPP)
CLANG_CXX := $(CLANG_EXE)

ifneq ($(USE_CLANG),0)
	CXX := $(CLANG_CXX)

	CXXFLAGS_DBG := $(CLANGFLAGS)
	CXXFLAGS_RELEASE := $(CLANGFLAGS_RELEASE)

	BUILD_SYSTEM_DEPS := $(BUILD_SYSTEM_DEPS) $(CLANG_DEPS)
endif

ifeq ($(USE_CCACHE),1)
	CXX := ccache $(CXX)
	CXX_PROFILE := ccache $(CXX_PROFILE)
	CLANG_CXX := ccache $(CLANG_CXX)
	ifeq ($(USE_DISTCC),1)
		CXX_ENV += CCACHE_PREFIX=distcc
	endif
else ifeq ($(USE_DISTCC),1)
	CXX := distcc $(CXX)
	CXX_PROFILE := distcc $(CXX_PROFILE)
	CLANG_CXX := distcc $(CLANG_CXX)
	LLVM_BUILD_VARS += CXX='distcc $(GPP)'
endif
ifeq ($(USE_DISTCC),1)
	LLVM_BUILD_ENV += CCACHE_PREFIX=distcc
endif
ifeq ($(USE_CCACHE),1)
	LLVM_BUILD_VARS += CXX='ccache $(GPP)'
endif
CXX := $(CXX_ENV) $(CXX)
CXX_PROFILE := $(CXX_ENV) $(CXX_PROFILE)
CLANG_CXX := $(CXX_ENV) $(CLANG_CXX)
# Not sure if ccache_basedir actually helps at all (I think the generated files make them different?)
LLVM_BUILD_ENV += CCACHE_DIR=$(HOME)/.ccache_llvm CCACHE_BASEDIR=$(LLVM_SRC)

MAIN_SRCS := $(wildcard codegen/*.cpp) $(wildcard asm_writing/*.cpp) $(wildcard codegen/irgen/*.cpp) $(wildcard codegen/opt/*.cpp) $(wildcard analysis/*.cpp) $(wildcard core/*.cpp) jit.cpp codegen/profiling/profiling.cpp codegen/profiling/dumprof.cpp $(wildcard runtime/*.cpp) $(wildcard runtime/builtin_modules/*.cpp) $(wildcard gc/*.cpp)
STDLIB_SRCS := $(wildcard runtime/inline/*.cpp)
SRCS := $(MAIN_SRCS) $(STDLIB_SRCS)
STDLIB_OBJS := stdlib.bc.o stdlib.stripped.bc.o
STDLIB_RELEASE_OBJS := stdlib.release.bc.o
# The stdlib objects have slightly longer dependency chains,
# so put them first in the list:
OBJS := $(STDLIB_OBJS) $(SRCS:.cpp=.o)
PROFILE_OBJS := $(STDLIB_RELEASE_OBJS) $(MAIN_SRCS:.cpp=.prof.o) $(STDLIB_SRCS:.cpp=.release.o)
OPT_OBJS := $(STDLIB_RELEASE_OBJS) $(SRCS:.cpp=.release.o)

OPTIONAL_SRCS := codegen/profiling/oprofile.cpp codegen/profiling/pprof.cpp
TOOL_SRCS := $(wildcard $(TOOLS_DIR)/*.cpp)

UNITTEST_DIR := ../test/unittests
UNITTEST_SRCS := $(wildcard $(UNITTEST_DIR)/*.cpp)

NONSTDLIB_SRCS := $(MAIN_SRCS) $(OPTIONAL_SRCS) $(TOOL_SRCS) $(UNITTEST_SRCS)

.DEFAULT_GOAL := pyston_dbg
# _ :
	# $(MAKE) pyston_dbg || (clear; $(MAKE) pyston_dbg -j1 ERROR_LIMIT=1)

.PHONY: all _all
all: llvm
	@# have to do this in a recursive make so that dependency is enforced:
	$(MAKE) pyston_all
pyston_all: pyston_dbg pyston pyston_oprof pyston_prof $(OPTIONAL_SRCS:.cpp=.o)

ALL_HEADERS := $(wildcard */*.h) $(wildcard */*/*.h)
tags: $(SRCS) $(OPTIONAL_SRCS) $(ALL_HEADERS)
	$(ECHO) Calculating tags...
	$(VERB) ctags $^

NON_ENTRY_OBJS := $(filter-out jit.o,$(OBJS))

define add_unittest
$(eval \
$(UNITTEST_DIR)/$1: $(GTEST_DIR)/src/gtest-all.o $(NON_ENTRY_OBJS) $(BUILD_SYSTEM_DEPS) $(UNITTEST_DIR)/$1.o
	$(ECHO) Linking $$@
	$(VERB) $(CXX) $(NON_ENTRY_OBJS) $(GTEST_DIR)/src/gtest-all.o $(GTEST_DIR)/src/gtest_main.o $(UNITTEST_DIR)/$1.o $(LDFLAGS) -o $$@
dbg_$1_unittests: $(UNITTEST_DIR)/$1
	zsh -c 'ulimit -v $(MAX_MEM_KB); ulimit -d $(MAX_MEM_KB); time $(GDB) $(GDB_CMDS) --args ./$(UNITTEST_DIR)/$1 --gtest_break_on_failure $(ARGS)'
run_$1_unittests: $(UNITTEST_DIR)/$1
	zsh -c 'ulimit -v $(MAX_MEM_KB); ulimit -d $(MAX_MEM_KB); time ./$(UNITTEST_DIR)/$1 $(ARGS)'
run_unittests:: run_$1_unittests
)
endef

override GDB_CMDS ?=
override GDB_CMDS := --ex "set confirm off" --ex "handle SIGUSR2 pass nostop noprint" --ex run --ex "bt 20" $(GDB_CMDS)
BR ?=
ARGS ?=
ifneq ($(BR),)
	override GDB_CMDS := --ex "break $(BR)" $(GDB_CMDS)
endif
$(call add_unittest,gc)
$(call add_unittest,analysis)


define checksha
	test "$$($1 | sha1sum)" = "$2  -"
endef

.PHONY: format check_format
format:
	find \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 $(LLVM_BIN)/clang-format -style=file -i
check_format:
	$(ECHO) checking formatting...
	$(VERB) $(TOOLS_DIR)/check_format.sh $(LLVM_BIN)/clang-format

.PHONY: analyze
analyze:
	$(MAKE) clean
	PATH=$$PATH:$(DEPS_DIR)/llvm-trunk/tools/clang/tools/scan-view $(DEPS_DIR)/llvm-trunk/tools/clang/tools/scan-build/scan-build \
		 --use-analyzer $(CLANG_EXE) --use-c++ $(CLANG_EXE) -V \
		$(MAKE) pyston_dbg USE_DISTCC=0 USE_CCACHE=0

.PHONY: lint cpplint
lint:
	$(ECHO) linting...
	$(VERB) python ../tools/lint.py
cpplint:
	$(VERB) python ../tools/cpplint.py --filter=-whitespace,-build/header_guard,-build/include_order,-readability/todo $(SRCS)

.PHONY: check quick_check
check:
	@# These are ordered roughly in decreasing order of (chance will expose issue) / (time to run test)
	$(MAKE) lint
	$(MAKE) ext pyston_dbg

	$(MAKE) check_dbg

	$(MAKE) check_format
	$(MAKE) run_unittests

	@# jit_prof forces the use of GCC as the compiler, which can expose other errors, so just build it and see what happens:
	# $(MAKE) check_prof
	$(MAKE) pyston_prof
	$(call checksha,./pyston_prof -cq  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -cqn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -cqO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)

	$(MAKE) check_release
	echo "All tests passed"

quick_check:
	$(MAKE) pyston_dbg
	$(call checksha,./pyston_dbg -cq  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_dbg -cqn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_dbg -cqO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(MAKE) pyston
	$(call checksha,./pyston -cq  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston -cqn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston -cqO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(MAKE) pyston_prof
	$(call checksha,./pyston_prof -cq  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -cqn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -cqO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)

Makefile.local:
	echo "Creating default Makefile.local"
	python -c 'import sys; v = sys.version_info; print "PYTHON_MAJOR_VERSION:=%d\nPYTHON_MINOR_VERSION:=%d\nPYTHON_MICRO_VERSION:=%d" % (v[0], v[1], v[2])' > Makefile.local || (rm $@; false)

#################
# LLVM rules:

#
# This is probably my worst makefile hackery:
# - if you change the patch, the llvm_* targets should be rebuilt when you build a pyston target that depends on them
# - they shouldn't be rebuilt if the built_* rule doesn't indicate it
# - should rebuild the pyston targets *if and only if* one of their dependencies actually changes in the rebuild
# -- make should report them as "up to date"

LLVM_CONFIGURATION := $(LLVM_BUILD)/Makefile.config
# First, specify when we need to rebuild the different targets:
$(LLVM_BUILD)/built_quick: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	$(MAKE) llvm_quick
$(LLVM_BUILD)/built_debug: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	$(MAKE) llvm_debug
$(LLVM_BUILD)/built_release: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	$(MAKE) llvm_release
$(LLVM_BUILD)/built_profile: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	$(MAKE) llvm_profile
# Now, specify that we shouldn't check the timestamps of dependencies until after
# the llvm rebuild finishes, if one is happening, but do it with order-only
# dependencies so that make doesn't consider the libraries out of date
# if they didn't get updated in the llvm rebuild:
# $(CLANG_EXE): | $(LLVM_TRUNK)/built_release
$(LLVM_LIB_DEPS): | $(LLVM_BUILD)/built_quick
$(LLVM_DEBUG_LIB_DEPS): | $(LLVM_BUILD)/built_debug
$(LLVM_RELEASE_LIB_DEPS): | $(LLVM_BUILD)/built_release
$(LLVM_PROFILE_LIB_DEPS): | $(LLVM_BUILD)/built_profile
# Now, put together some variables for later; pyston targets will have to depend on the lib_deps
# so they can be rebuilt properly, but also the built_* targets to force a rebuild if appropriate
# (because the lib_deps dependency won't cause a rebuild on their own)
LLVM_DEPS := $(LLVM_LIB_DEPS) $(LLVM_BUILD)/built_quick
LLVM_DEBUG_DEPS := $(LLVM_DEBUG_LIB_DEPS) $(LLVM_BUILD)/built_debug
LLVM_RELEASE_DEPS := $(LLVM_RELEASE_LIB_DEPS) $(LLVM_BUILD)/built_release
LLVM_PROFILE_DEPS := $(LLVM_PROFILE_LIB_DEPS) $(LLVM_BUILD)/built_profile
# end worst makefile hackery

LLVM_BUILDS := quick release debug profile
# Tools for all builds (note: don't include llvm-config)
LLVM_TOOLS := llc opt

ifneq (,$(findstring llvm_debug,$(MAKECMDGOALS)))
FIRST_LLVM_BUILD := debug
else ifneq (,$(findstring llvm_quick,$(MAKECMDGOALS)))
FIRST_LLVM_BUILD := quick
else
FIRST_LLVM_BUILD := release
endif
NONFIRST_LLVM_BUILDS := $(filter-out $(FIRST_LLVM_BUILD),$(LLVM_BUILDS))
.PHONY: llvm llvm_configs $(patsubst %,llvm_%,$(LLVM_BUILDS)) llvm/% llvm_up
llvm: llvm_configs $(LLVM_BUILDS:%=llvm_%)
llvm_configs: $(LLVM_BUILDS:%=llvm/%/tools/llvm-config)
# Use the configure-created Makefile as evidence that llvm has been configured:
.PHONY: llvm_configure
llvm_configure:
	rm -f $(LLVM_BUILD)/Makefile.config
	$(MAKE) $(LLVM_CONFIGURATION)

LLVM_CONFIGURE_LINE := CXX=$(GPP) $(LLVM_SRC)/configure --enable-targets=host --with-gcc-toolchain=$(GCC_DIR)
ifneq ($(ENABLE_INTEL_JIT_EVENTS),0)
LLVM_CONFIGURE_LINE += --with-intel-jitevents
endif
$(LLVM_CONFIGURATION): $(LLVM_SRC)/configure | $(LLVM_SRC)/_patched
	mkdir -p $(LLVM_BUILD)
	cd $(LLVM_BUILD) ; \
	$(LLVM_CONFIGURE_LINE)
	# CXX=ccache\ g++ ./configure --enable-targets=host
	# CXX='env CCACHE_PREFIX=distcc ccache g++' ./configure --enable-targets=host

# Use "Static Pattern Rules" instead of implicit rules to avoid needing to reuse the same implicit rule in a single chain:
define add_llvm_dep
$(eval \
$(LLVM_BUILDS:%=llvm/%/$1): llvm/%/$1: llvm/%/$2
)
endef

###
# LLVM build dependency management:
$(call add_llvm_dep,lib/TableGen,lib/Support)
$(call add_llvm_dep,utils/TableGen,lib/TableGen)
$(call add_llvm_dep,lib/IR,utils/TableGen)
$(call add_llvm_dep,lib/Target,lib/IR)
$(call add_llvm_dep,lib/Target,utils/TableGen)
# There are some shared targets in the Target subdirectory, which will make a parallel make fail
# if you try to build multiple llvm builds at the same time.  Work around this by
# serializing the non-release Target builds to after the release one:
$(NONFIRST_LLVM_BUILDS:%=llvm/%/lib/Target): llvm/%/lib/Target: llvm/$(FIRST_LLVM_BUILD)/lib/Target
$(NONFIRST_LLVM_BUILDS:%=llvm/%/tools/llvm-config): llvm/%/tools/llvm-config: llvm/$(FIRST_LLVM_BUILD)/tools/llvm-config
$(call add_llvm_dep,lib,lib/Target)
$(call add_llvm_dep,tools/llvm-config,lib/Support)
$(call add_llvm_dep,tools,lib)
$(call add_llvm_dep,tools,utils/unittest)
$(call add_llvm_dep,utils,lib)
# The tools need to individually depend on the lib directory:
$(foreach tool,$(LLVM_TOOLS),$(foreach build,$(LLVM_BUILDS),$(eval \
llvm/$(build)/tools/$(tool): llvm/$(build)/lib \
)))

##
# LLVM build subset specifications:
$(LLVM_BUILDS:%=llvm_%): llvm_%: llvm/%/lib llvm/%/tools/llvm-config
	touch $(LLVM_BUILD)/$(patsubst llvm_%,built_%,$@)
llvm_release: llvm/release/tools llvm/release/utils
llvm_quick: $(LLVM_TOOLS:%=llvm/quick/tools/%)
llvm_debug: $(LLVM_TOOLS:%=llvm/debug/tools/%)

llvm_quick_clean:
	$(MAKE) -C $(LLVM_BUILD) ENABLE_OPTIMIZED=1 clean
llvm_release_%:
	$(MAKE) -C $(LLVM_BUILD) ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 $(patsubst llvm_release_%,%,$@)
llvm_debug_clean:
	$(MAKE) -C $(LLVM_BUILD) DEBUG_RUNTIME=1 DEBUG_SYMBOLS=1 ENABLE_OPTIMIZED=0 clean
llvm_profile_clean:
	$(MAKE) -C $(LLVM_BUILD) ENABLE_PROFILING=1 ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 clean
llvm_allclean: $(patsubst %,llvm_%_clean,$(LLVM_BUILDS))
	cd $(LLVM_SRC) ; git checkout .
	rm -rfv $(LLVM_BUILD)/*
llvm_install: llvm_release
	sudo $(MAKE) -C $(LLVM_SRC)/tools ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 install

# Clear OPTIONAL_DIRS and OPTIONAL_PARALLEL_DIRS to make sure that clang doesn't get built+tested
llvm_test: llvm_test_quick
	$(MAKE) llvm_test_release
llvm_test_quick: llvm_test_quick
	$(MAKE) -C $(LLVM_BUILD) OPTIONAL_DIRS= OPTIONAL_PARALLEL_DIRS= ENABLE_OPTIMIZED=1 check
llvm_test_release: llvm_release
	$(MAKE) -C $(LLVM_BUILD) OPTIONAL_DIRS= OPTIONAL_PARALLEL_DIRS= ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 check
llvm_test_all: llvm_release
	$(MAKE) -C $(LLVM_BUILD) ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 check-all

llvm/quick/%: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	mkdir -p $(patsubst llvm/quick/%,$(LLVM_BUILD)/%,$@)
	$(VERB) if [ ! -f $(patsubst llvm/quick/%,$(LLVM_BUILD)/%/Makefile,$@) ]; then cp $(patsubst llvm/quick/%,$(LLVM_SRC)/%/Makefile,$@) $(patsubst llvm/quick/%,$(LLVM_BUILD)/%/,$@); fi
	$(LLVM_BUILD_ENV) $(MAKE) -C $(patsubst llvm/quick/%,$(LLVM_BUILD)/%,$@) $(LLVM_BUILD_VARS) ENABLE_OPTIMIZED=1 DEBUG_RUNTIME=0 NO_DEBUG_SYMBOLS=1
llvm/release/%: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	mkdir -p $(patsubst llvm/release/%,$(LLVM_BUILD)/%,$@)
	$(VERB) if [ ! -f $(patsubst llvm/release/%,$(LLVM_BUILD)/%/Makefile,$@) ]; then cp $(patsubst llvm/release/%,$(LLVM_SRC)/%/Makefile,$@) $(patsubst llvm/release/%,$(LLVM_BUILD)/%/,$@); fi
	$(LLVM_BUILD_ENV) $(MAKE) -C $(patsubst llvm/release/%,$(LLVM_BUILD)/%,$@) $(LLVM_BUILD_VARS) ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1
llvm/debug/%: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	mkdir -p $(patsubst llvm/debug/%,$(LLVM_BUILD)/%,$@)
	$(VERB) if [ ! -f $(patsubst llvm/debug/%,$(LLVM_BUILD)/%/Makefile,$@) ]; then cp $(patsubst llvm/debug/%,$(LLVM_SRC)/%/Makefile,$@) $(patsubst llvm/debug/%,$(LLVM_BUILD)/%/,$@); fi
	$(LLVM_BUILD_ENV) $(MAKE) -C $(patsubst llvm/debug/%,$(LLVM_BUILD)/%,$@) $(LLVM_BUILD_VARS) DEBUG_RUNTIME=1 DEBUG_SYMBOLS=1 ENABLE_OPTIMIZED=0
llvm/profile/%: $(LLVM_SRC)/_patched $(LLVM_CONFIGURATION)
	mkdir -p $(patsubst llvm/profile/%,$(LLVM_BUILD)/%,$@)
	$(VERB) if [ ! -f $(patsubst llvm/profile/%,$(LLVM_BUILD)/%/Makefile,$@) ]; then cp $(patsubst llvm/profile/%,$(LLVM_SRC)/%/Makefile,$@) $(patsubst llvm/profile/%,$(LLVM_BUILD)/%/,$@); fi
	$(LLVM_BUILD_ENV) $(MAKE) -C $(patsubst llvm/profile/%,$(LLVM_BUILD)/%,$@) $(LLVM_BUILD_VARS) CXXFLAGS="-fno-omit-frame-pointer -fno-function-sections" ENABLE_PROFILING=1 ENABLE_OPTIMIZED=1 DISABLE_ASSERTIONS=1 $(LLVM_MAKE_ARGS)

$(LLVM_SRC)/_patched: $(wildcard llvm_patches/*) $(wildcard clang_patches/*) $(LLVM_REVISION_FILE)
	$(MAKE) llvm_up
llvm_up:
	python $(TOOLS_DIR)/git_svn_gotorev.py $(LLVM_SRC) $(LLVM_REVISION) ../llvm_patches
	python $(TOOLS_DIR)/git_svn_gotorev.py $(LLVM_SRC)/tools/clang $(LLVM_REVISION) ../clang_patches
	touch $(LLVM_SRC)/_patched

# end of llvm rules
########

## TOOLS:

$(TOOLS_DIR)/demangle: $(TOOLS_DIR)/demangle.cpp $(BUILD_SYSTEM_DEPS)
	$(CXX) $< -o $@
.PHONY: demangle
demangle: $(TOOLS_DIR)/demangle
	$(TOOLS_DIR)/demangle $(ARGS)

$(TOOLS_DIR)/mcjitcache: $(TOOLS_DIR)/mcjitcache.o $(LLVM_DEPS) $(BUILD_SYSTEM_DEPS)
	$(CXX) $< $(LDFLAGS) -o $@
# Build a version of mcjitcache off the llvm_release repo mostly to avoid a dependence of the opt builds
# on the llvm_quick build.
$(TOOLS_DIR)/mcjitcache_release: $(TOOLS_DIR)/mcjitcache.release.o $(LLVM_RELEASE_DEPS) $(BUILD_SYSTEM_DEPS)
	$(CXX) $< $(LDFLAGS_RELEASE) -o $@

$(TOOLS_DIR)/publicize: $(TOOLS_DIR)/publicize.o $(LLVM_DEPS) $(BUILD_SYSTEM_DEPS)
	$(ECHO) Linking $(TOOLS_DIR)/publicize
	$(VERB) $(CXX) $< $(LDFLAGS) -o $@ -lLLVMBitWriter

$(TOOLS_DIR)/publicize_release: $(TOOLS_DIR)/publicize.release.o $(LLVM_RELEASE_DEPS) $(BUILD_SYSTEM_DEPS)
	$(ECHO) Linking $(TOOLS_DIR)/publicize_release
	$(VERB) $(CXX) $< $(LDFLAGS_RELEASE) -o $@ -lLLVMBitWriter

## END OF TOOLS


CODEGEN_SRCS := $(wildcard codegen/*.cpp) $(wildcard codegen/*/*.cpp)

# args: suffix (ex: ".release"), CXXFLAGS
define make_compile_config
$(eval \
$$(NONSTDLIB_SRCS:.cpp=$1.o): CXXFLAGS:=$2
$$(SRCS:.cpp=$1.o.bc): CXXFLAGS:=$2

## Need to set CXXFLAGS so that this rule doesn't inherit the '-include' rule from the
## thing that's calling it:
%$1.h.pch: CXXFLAGS:=$(2)
%$1.h.pch: %.h $$(BUILD_SYSTEM_DEPS)
	$$(ECHO) Compiling $$@
	$$(VERB) rm -f $$@-*
	$$(VERB) $$(CLANG_EXE) $$(CXXFLAGS) -MMD -MP -MF $$<.d -x c++-header $$< -o $$@
$$(CODEGEN_SRCS:.cpp=$1.o): CXXFLAGS += -include codegen/irgen$1.h
$$(CODEGEN_SRCS:.cpp=$1.o): codegen/irgen$1.h.pch

$$(NONSTDLIB_SRCS:.cpp=$1.o): %$1.o: %.cpp $$(BUILD_SYSTEM_DEPS)
	$$(ECHO) Compiling $$@
	$$(VERB) $$(CXX) $$(CXXFLAGS) -MMD -MP -MF $$(patsubst %.o,%.d,$$@) $$< -c -o $$@

# For STDLIB sources, first compile to bitcode:
$$(STDLIB_SRCS:.cpp=$1.o.bc): %$1.o.bc: %.cpp $$(BUILD_SYSTEM_DEPS) $$(CLANG_DEPS)
	$$(ECHO) Compiling $$@
	$$(VERB) $$(CLANG_CXX) $$(CXXFLAGS) $$(CLANG_EXTRA_FLAGS) -MMD -MP -MF $$(patsubst %.bc,%.d,$$@) $$< -c -o $$@ -emit-llvm -g

stdlib$1.unopt.bc: $$(STDLIB_SRCS:.cpp=$1.o.pub.bc)
	$$(ECHO) Linking $$@
	$$(VERB) $$(LLVM_BIN)/llvm-link $$^ -o $$@

)
endef

PASS_SRCS := codegen/opt/aa.cpp
PASS_OBJS := $(PASS_SRCS:.cpp=.standalone.o)

$(call make_compile_config,,$(CXXFLAGS_DBG))
$(call make_compile_config,.release,$(CXXFLAGS_RELEASE))
$(call make_compile_config,.grwl,$(CXXFLAGS_RELEASE) -DTHREADING_USE_GRWL=1 -DTHREADING_USE_GIL=0 -UBINARY_SUFFIX -DBINARY_SUFFIX=_grwl)
$(call make_compile_config,.grwl_dbg,$(CXXFLAGS_DBG) -DTHREADING_USE_GRWL=1 -DTHREADING_USE_GIL=0 -UBINARY_SUFFIX -DBINARY_SUFFIX=_grwl_dbg -UBINARY_STRIPPED_SUFFIX -DBINARY_STRIPPED_SUFFIX=)
$(call make_compile_config,.nosync,$(CXXFLAGS_RELEASE) -DTHREADING_USE_GRWL=0 -DTHREADING_USE_GIL=0 -UBINARY_SUFFIX -DBINARY_SUFFIX=_nosync)

$(UNITTEST_SRCS:.cpp=.o): CXXFLAGS += -isystem $(GTEST_DIR)/include

$(PASS_SRCS:.cpp=.standalone.o): %.standalone.o: %.cpp $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling $@
	$(VERB) $(CXX) $(CXXFLAGS_DBG) -DSTANDALONE -MMD -MP -MF $(patsubst %.o,%.d,$@) $< -c -o $@
$(NONSTDLIB_SRCS:.cpp=.prof.o): %.prof.o: %.cpp $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling $@
	$(VERB) $(CXX_PROFILE) $(CXXFLAGS_PROFILE) -MMD -MP -MF $(patsubst %.o,%.d,$@) $< -c -o $@

# Then, publicize symbols:
%.pub.bc: %.bc $(TOOLS_DIR)/publicize
	$(ECHO) Publicizing $<
	$(VERB) $(TOOLS_DIR)/publicize $< -o $@

# Then, compile the publicized bitcode into normal .o files
%.o: %.o.pub.bc $(BUILD_SYSTEM_DEPS) $(CLANG_DEPS)
	$(ECHO) Compiling bitcode to $@
	$(VERB) $(LLVM_BIN)/clang $(CLANGFLAGS) -O3 -c $< -o $@



passes.so: $(PASS_OBJS) $(BUILD_SYSTEM_DEPS)
	$(CXX) -shared $(PASS_OBJS) -o $@ -shared -rdynamic
test_opt: passes.so
	$(LLVM_BUILD)/Release+Asserts/bin/opt -load passes.so test.ll -S -o test.opt.ll $(ARGS)
test_dbg_opt: passes.so
	$(GDB) --args $(LLVM_BUILD)/Release+Asserts/bin/opt -O3 -load passes.so test.ll -S -o test.opt.ll $(ARGS)


# Optimize and/or strip it:
stdlib.bc: OPT_OPTIONS=-O3
stdlib.release.bc: OPT_OPTIONS=-O3 -strip-debug
.PRECIOUS: %.bc %.stripped.bc
%.bc: %.unopt.bc
	$(ECHO) Optimizing $< -\> $@
	$(VERB) $(LLVM_BIN)/opt $(OPT_OPTIONS) $< -o $@
%.stripped.bc: %.bc
	$(ECHO) Stripping $< -\> $@
	$(VERB) $(LLVM_BIN)/opt -strip-debug $< -o $@

# Then do "ld -b binary" to create a .o file for embedding into the executable
# $(STDLIB_OBJS) $(STDLIB_RELEASE_OBJS): %.o: % $(BUILD_SYSTEM_DEPS)
stdli%.bc.o: stdli%.bc $(BUILD_SYSTEM_DEPS)
	$(ECHO) Embedding $<
	$(VERB) ld -r -b binary $< -o $@

# Optionally, disassemble the bitcode files:
%.ll: %.bc
	$(LLVM_BIN)/llvm-dis $<

# Not used currently, but here's how to pre-jit the stdlib bitcode:
%.release.cache: %.release.bc mcjitcache_release
	./mcjitcache_release -p $< -o $@
%.cache: %.bc mcjitcache
	./mcjitcache -p $< -o $@

# args: output suffx (ex: _release), objects to link, LDFLAGS, other deps
define link
$(eval \
pyston$1: $2 $4
	$$(ECHO) Linking $$@
	$$(VERB) $$(CXX) $2 $3 -o $$@
)
endef


# Finally, link it all together:
$(call link,_dbg,$(OBJS),$(LDFLAGS),$(LLVM_DEPS))
$(call link,_debug,$(OBJS),$(LDFLAGS_DEBUG),$(LLVM_DEBUG_DEPS))
$(call link,_release,$(OPT_OBJS),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))
$(call link,_grwl,stdlib.grwl.bc.o $(SRCS:.cpp=.grwl.o),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))
$(call link,_grwl_dbg,stdlib.grwl_dbg.bc.o $(SRCS:.cpp=.grwl_dbg.o),$(LDFLAGS),$(LLVM_DEPS))
$(call link,_nosync,stdlib.nosync.bc.o $(SRCS:.cpp=.nosync.o),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))
pyston_oprof: $(OPT_OBJS) codegen/profiling/oprofile.o $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(OPT_OBJS) codegen/profiling/oprofile.o $(LDFLAGS_RELEASE) -lopagent -o $@
pyston_pprof: $(OPT_OBJS) codegen/profiling/pprof.release.o $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(OPT_OBJS) codegen/profiling/pprof.release.o $(LDFLAGS_RELEASE) -lprofiler -o $@
pyston_prof: $(PROFILE_OBJS) $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(PROFILE_OBJS) $(LDFLAGS) -pg -o $@
pyston_profile: $(PROFILE_OBJS) $(LLVM_PROFILE_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(PROFILE_OBJS) $(LDFLAGS_PROFILE) -o $@


-include $(wildcard *.d) $(wildcard */*.d) $(wildcard */*/*.d) $(wildcard $(UNITTEST_DIR)/*.d)

.PHONY: clean
clean:
	@ find . $(TOOLS_DIR) ../test \( -name '*.o' -o -name '*.d' -o -name '*.py_cache' -o -name '*.bc' -o -name '*.o.ll' -o -name '*.pub.ll' -o -name '*.cache' -o -name 'stdlib*.ll' -o -name '*.pyc' -o -name '*.so' -o -name '*.a' -o -name '*.expected_cache' -o -name '*.pch' \) -print -delete
	@ find \( -name 'pyston*' -executable -type f \) -print -delete
	@ find $(TOOLS_DIR) -maxdepth 0 -executable -type f -print -delete
	@ rm -rf oprofile_data

# A helper function that lets me run subdirectory rules from the top level;
# ex instead of saying "make tests/run_1", I can just write "make run_1"
define make_search
$(eval \
$1: ../test/tests/$1 ;
$1: ../microbenchmarks/$1 ;
$1: ../millibenchmarks/$1 ;
$1: ../benchmarks/$1 ;
)
endef

RUN_DEPS := ext

define make_target
$(eval \
.PHONY: test$1 check$1
check$1 test$1: pyston$1 ext
	python ../tools/tester.py -R pyston$1 -j$(TEST_THREADS) -k $(TESTS_DIR) $(ARGS)
	python ../tools/tester.py -R pyston$1 -j$(TEST_THREADS) -a -n -k $(TESTS_DIR) $(ARGS)
	python ../tools/tester.py -R pyston$1 -j$(TEST_THREADS) -a -O -k $(TESTS_DIR) $(ARGS)

.PHONY: run$1 dbg$1
run$1: pyston$1 $$(RUN_DEPS)
	./pyston$1 $$(ARGS)
dbg$1: pyston$1 $$(RUN_DEPS)
	zsh -c 'ulimit -v $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS)'
run$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) zsh -c 'ulimit -v $$(MAX_MEM_KB); ulimit -d $$(MAX_MEM_KB); time ./pyston$1 $$(ARGS) $$<'
$$(call make_search,run$1_%)
dbg$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) zsh -c 'ulimit -v $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS) $$<'
$$(call make_search,dbg$1_%)

ifneq ($$(ENABLE_VALGRIND),0)
memcheck$1_%: %.py pyston$1 $$(RUN_DEPS)
	$$(VALGRIND) --tool=memcheck --leak-check=no --db-attach=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memcheck$1_%)
memcheck_gdb$1_%: %.py pyston$1 $$(RUN_DEPS)
	set +e; $$(VALGRIND) -v -v -v -v -v --tool=memcheck --leak-check=no --track-origins=yes --vgdb=yes --vgdb-error=0 ./pyston$1 $$(ARGS) $$< & export PID=$$$$! ; \
	$$(GDB) --ex "set confirm off" --ex "target remote | $$(DEPS_DIR)/valgrind-3.9.0-install/bin/vgdb" --ex "continue" --ex "bt" ./pyston$1; kill -9 $$$$PID
$$(call make_search,memcheck_gdb$1_%)
memleaks$1_%: %.py pyston$1 $$(RUN_DEPS)
	$$(VALGRIND) --tool=memcheck --leak-check=full --leak-resolution=low --show-reachable=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memleaks$1_%)
cachegrind$1_%: %.py pyston$1 $$(RUN_DEPS)
	$$(VALGRIND) --tool=cachegrind ./pyston$1 $$(ARGS) $$<
$$(call make_search,cachegrind$1_%)
endif

.PHONY: perf$1_%
perf$1_%: %.py pyston$1
	perf record -g -- ./pyston$1 -q -p $$(ARGS) $$<
	perf report -v -n -g flat,1000 | bash $$(TOOLS_DIR)/cumulate.sh | less -S
$$(call make_search,perf$1_%)

)
endef

.PHONY: run run_% dbg_% debug_% perf_%
run: run_dbg
run_%: run_dbg_%
	@true
dbg_%: dbg_dbg_%
	@true
debug_%: dbg_debug_%
	@true
perf_%: perf_release_%
	@true

$(call make_target,_dbg)
$(call make_target,_release)
$(call make_target,_grwl)
$(call make_target,_grwl_dbg)
$(call make_target,_nosync)

# "kill valgrind":
kv:
	ps aux | awk '/[v]algrind/ {print $$2}' | xargs kill -9; true

# gprof-based profiling:
.PHONY: prof_% profile_%
prof_%: %.py pyston_prof
	zsh -c 'time ./pyston_prof $(ARGS) $<'
	gprof ./pyston_prof gmon.out > $(patsubst %,%.out,$@)
$(call make_search,prof_%)
profile_%: %.py pyston_profile
	time ./pyston_profile -p $(ARGS) $<
	gprof ./pyston_profile gmon.out > $(patsubst %,%.out,$@)
$(call make_search,profile_%)

# pprof-based profiling:
.PHONY: pprof_% pprof_release_%
pprof_%: %.py pyston_pprof
	CPUPROFILE_FREQUENCY=1000 CPUPROFILE=$@.out ./pyston_pprof -p $(ARGS) $<
	pprof --raw pyston_pprof $@.out > $@_raw.out
	python codegen/profiling/process_pprof.py $@_raw.out pprof.jit > $@_processed.out
	pprof --text $@_processed.out
	# rm -f pprof.out pprof.raw pprof.jit
$(call make_search,pprof_%)

# oprofile-based profiling:
.PHONY: oprof_% oprof_collect_% opreport
oprof_collect_%: %.py pyston_oprof
	sudo opcontrol --image pyston_oprof
	# sudo opcontrol --event CPU_CLK_UNHALTED:28000
	# sudo opcontrol --cpu-buffer-size=128000 --buffer-size=1048576 --buffer-watershed=1048000
	sudo opcontrol --reset
	sudo opcontrol --start
	time ./pyston_oprof -p $(ARGS) $<
	sudo opcontrol --stop
	sudo opcontrol --dump
	sudo opcontrol --image all --event default --cpu-buffer-size=0 --buffer-size=0 --buffer-watershed=0
	sudo opcontrol --deinit
	sudo opcontrol --init
oprof_%: oprof_collect_%
	$(MAKE) opreport
$(call make_search,oprof_%)
opreport:
	! [ -d oprofile_data ]
	opreport -l -t 0.2 -a pyston_oprof
	# opreport lib-image:pyston_oprof -l -t 0.2 -a | head -n 25

.PHONY: oprofcg_% oprof_collectcg_% opreportcg
oprof_collectcg_%: %.py pyston_oprof
	operf -g -e CPU_CLK_UNHALTED:90000 ./pyston_oprof -p $(ARGS) $<
oprofcg_%: oprof_collectcg_%
	$(MAKE) opreportcg
$(call make_search,oprofcg_%)
opreportcg:
	opreport lib-image:pyston_oprof -l -t 0.2 -a --callgraph

.PHONY: watch_% watch wdbg_%
watch_%:
	@ ( ulimit -t 60; ulimit -d $(MAK_MEM_KB); ulimit -v $(MAK_MEM_KB); \
		TARGET=$(dir $@)$(patsubst watch_%,%,$(notdir $@)); \
		clear; $(MAKE) $$TARGET $(WATCH_ARGS); true; \
		while inotifywait -q -e modify -e attrib -e move -e move_self -e create -e delete -e delete_self \
		Makefile $$(find .. \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) ); do clear; $(MAKE) $$TARGET $(WATCH_ARGS); done )
		# Makefile $$(find \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) -o -type d ); do clear; $(MAKE) $(patsubst watch_%,%,$@); done )
		# -r . ; do clear; $(MAKE) $(patsubst watch_%,%,$@); done
watch: watch_pyston_dbg
watch_vim:
	$(MAKE) watch WATCH_ARGS='COLOR=0 USE_DISTCC=0 -j1 2>&1 | tee compile.log'
wdbg_%:
	$(MAKE) $(patsubst wdbg_%,watch_dbg_%,$@) GDB_CMDS="--ex quit"

.PHONY: test_asm test_cpp_asm
test_asm:
	$(CLANG_EXE) ../test/test.s -c -o test
	objdump -d test | less
	@ rm test
test_cpp_asm:
	$(CLANG_EXE) ../test/test.cpp -o test -c -O3 -std=c++11
	# $(GPP) tests/test.cpp -o test -c -O3
	objdump -d test | less
	rm test
test_cpp_ll:
	$(CLANG_EXE) ../test/test.cpp -o test.ll -c -O3 -emit-llvm -S -std=c++11
	less test.ll
	rm test.ll

.PHONY: ext
ext: ../test/test_extension/test.so
../test/test_extension/test.so: ../test/test_extension/test.o $(BUILD_SYSTEM_DEPS)
	cc -shared $< -o $@ -g
../test/test_extension/test.o: ../test/test_extension/test.c $(wildcard ../include/*.h) $(BUILD_SYSTEM_DEPS)
	cc -O2 -fPIC -Wimplicit -I../include -c $< -o $@ -g
