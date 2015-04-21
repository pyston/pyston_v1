# Disable builtin rules:
.SUFFIXES:

USE_TEST_LLVM := 0
DEPS_DIR := $(HOME)/pyston_deps

LLVM_REVISION_FILE := llvm_revision.txt
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

TEST_THREADS := 1

ERROR_LIMIT := 10
COLOR := 1

SELF_HOST := 0

VERBOSE := 0

ENABLE_INTEL_JIT_EVENTS := 0

CTAGS := ctags
ETAGS := ctags-exuberant -e

# Setting this to 1 will set the Makefile to use binaries from the trunk
# directory, even if USE_TEST_LLVM is set to 1.
# This is useful if clang isn't installed into the test directory, ex due
# to disk space concerns.
FORCE_TRUNK_BINARIES := 0

USE_CMAKE := 0
NINJA := ninja

# Put any overrides in here:
-include Makefile.local


ifneq ($(SELF_HOST),1)
	PYTHON := python
	PYTHON_EXE_DEPS :=
else
	PYTHON := $(abspath ./pyston_dbg)
	PYTHON_EXE_DEPS := pyston_dbg
endif

TOOLS_DIR := ./tools
TEST_DIR := ./test
TESTS_DIR := ./test/tests

GPP := $(GCC_DIR)/bin/g++
GCC := $(GCC_DIR)/bin/gcc

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

LLVM_LINK_LIBS := core mcjit native bitreader ipo irreader debuginfodwarf instrumentation
ifneq ($(ENABLE_INTEL_JIT_EVENTS),0)
LLVM_LINK_LIBS += inteljitevents
endif

NEED_OLD_JIT := $(shell if [ $(LLVM_REVISION) -le 216982 ]; then echo 1; else echo 0; fi )
ifeq ($(NEED_OLD_JIT),1)
	LLVM_LINK_LIBS += jit
endif

LLVM_CXXFLAGS := $(shell $(LLVM_BUILD)/Release+Asserts/bin/llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_BUILD)/Release+Asserts/bin/llvm-config --ldflags --system-libs --libs $(LLVM_LINK_LIBS))
LLVM_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Release+Asserts/lib/*)

LLVM_DEBUG_LDFLAGS := $(shell $(LLVM_BUILD)/Debug+Asserts/bin/llvm-config --ldflags --system-libs --libs $(LLVM_LINK_LIBS))
LLVM_DEBUG_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Debug+Asserts/lib/*)

LLVM_CONFIG_RELEASE := $(LLVM_BUILD)/Release/bin/llvm-config
ifneq ($(wildcard $(LLVM_CONFIG_RELEASE)),)
LLVM_RELEASE_CXXFLAGS := $(shell $(LLVM_BUILD)/Release/bin/llvm-config --cxxflags)
LLVM_RELEASE_LDFLAGS := $(shell $(LLVM_BUILD)/Release/bin/llvm-config --ldflags --system-libs --libs $(LLVM_LINK_LIBS))
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
LLVM_PROFILE_LDFLAGS = $(shell $(LLVM_BUILD)/Release+Profile/bin/llvm-config --ldflags --system-libs --libs $(LLVM_LINK_LIBS))
LLVM_PROFILE_LIB_DEPS := $(wildcard $(LLVM_BUILD)/Release+Profile/lib/*)

CLANG_EXE := $(LLVM_BIN)/clang
CLANGPP_EXE := $(LLVM_BIN)/clang++

COMMON_CFLAGS := -g -Werror -Wreturn-type -Wall -Wno-sign-compare -Wno-unused -Isrc -Ifrom_cpython/Include -fno-omit-frame-pointer
COMMON_CFLAGS += -Wextra -Wno-sign-compare
COMMON_CFLAGS += -Wno-unused-parameter # should use the "unused" attribute
COMMON_CXXFLAGS := $(COMMON_CFLAGS)
COMMON_CXXFLAGS += -std=c++11
COMMON_CXXFLAGS += -Woverloaded-virtual
COMMON_CXXFLAGS += -fexceptions -fno-rtti
COMMON_CXXFLAGS += -Wno-invalid-offsetof # allow the use of "offsetof", and we'll just have to make sure to only use it legally.
COMMON_CXXFLAGS += -DENABLE_INTEL_JIT_EVENTS=$(ENABLE_INTEL_JIT_EVENTS)
COMMON_CXXFLAGS += -I$(DEPS_DIR)/pypa-install/include
COMMON_CXXFLAGS += -I$(DEPS_DIR)/lz4-install/include

ifeq ($(ENABLE_VALGRIND),0)
	COMMON_CXXFLAGS += -DNVALGRIND
	VALGRIND := false
else
	COMMON_CXXFLAGS += -I$(DEPS_DIR)/valgrind-3.10.0/include
	VALGRIND := VALGRIND_LIB=$(DEPS_DIR)/valgrind-3.10.0-install/lib/valgrind $(DEPS_DIR)/valgrind-3.10.0-install/bin/valgrind
endif

COMMON_CXXFLAGS += -DGITREV=$(shell git rev-parse HEAD | head -c 12) -DLLVMREV=$(LLVM_REVISION)
COMMON_CXXFLAGS += -DDEFAULT_PYTHON_MAJOR_VERSION=$(PYTHON_MAJOR_VERSION) -DDEFAULT_PYTHON_MINOR_VERSION=$(PYTHON_MINOR_VERSION) -DDEFAULT_PYTHON_MICRO_VERSION=$(PYTHON_MICRO_VERSION)

# Use our "custom linker" that calls gold if available
COMMON_LDFLAGS := -B$(TOOLS_DIR)/build_system -L/usr/local/lib -lpthread -lm -lunwind -llzma -L$(DEPS_DIR)/gcc-4.8.2-install/lib64 -lreadline -lgmp -lssl -lcrypto -lsqlite3
COMMON_LDFLAGS += $(DEPS_DIR)/pypa-install/lib/libpypa.a
COMMON_LDFLAGS += $(DEPS_DIR)/lz4-install/lib/liblz4.a

# Conditionally add libtinfo if available - otherwise nothing will be added
COMMON_LDFLAGS += `pkg-config tinfo 2>/dev/null && pkg-config tinfo --libs || echo ""`

# Make sure that we put all symbols in the dynamic symbol table so that MCJIT can load them;
# TODO should probably do the linking before MCJIT
COMMON_LDFLAGS += -Wl,-E

# We get multiple shared libraries (libstdc++, libgcc_s) from the gcc installation:
ifneq ($(GCC_DIR),/usr)
	COMMON_LDFLAGS += -Wl,-rpath $(GCC_DIR)/lib64
endif

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
CXXFLAGS_RELEASE := $(LLVM_RELEASE_CXXFLAGS) $(COMMON_CXXFLAGS) -O3 -fstrict-aliasing -DNDEBUG -DNVALGRIND -DBINARY_SUFFIX=_release -DBINARY_STRIPPED_SUFFIX= $(EXTRA_CXXFLAGS)

LDFLAGS := $(LLVM_LDFLAGS) $(COMMON_LDFLAGS)
LDFLAGS_DEBUG := $(LLVM_DEBUG_LDFLAGS) $(COMMON_LDFLAGS)
LDFLAGS_PROFILE = $(LLVM_PROFILE_LDFLAGS) -pg $(COMMON_LDFLAGS)
LDFLAGS_RELEASE := $(LLVM_RELEASE_LDFLAGS) $(COMMON_LDFLAGS)
# Can't add this, because there are functions in the compiler that look unused but are hooked back from the runtime:
# LDFLAGS_RELEASE += -Wl,--gc-sections


BUILD_SYSTEM_DEPS := Makefile Makefile.local $(wildcard build_system/*)
CLANG_DEPS := $(CLANGPP_EXE) $(abspath $(dir $(CLANGPP_EXE))/../../built_release)
$(CLANGPP_EXE) $(CLANG_EXE): $(abspath $(dir $(CLANGPP_EXE))/../../built_release)

# settings to make clang and ccache play nicely:
CLANG_CCACHE_FLAGS := -Qunused-arguments
CLANG_EXTRA_FLAGS := -enable-tbaa -ferror-limit=$(ERROR_LIMIT) $(CLANG_CCACHE_FLAGS)
ifeq ($(COLOR),1)
	CLANG_EXTRA_FLAGS += -fcolor-diagnostics
else
	CLANG_EXTRA_FLAGS += -fno-color-diagnostics
endif
CLANGFLAGS := $(CXXFLAGS_DBG) $(CLANG_EXTRA_FLAGS)
CLANGFLAGS_RELEASE := $(CXXFLAGS_RELEASE) $(CLANG_EXTRA_FLAGS)

EXT_CFLAGS := $(COMMON_CFLAGS) -fPIC -Wimplicit -O2 -Ifrom_cpython/Include
EXT_CFLAGS += -Wno-missing-field-initializers
EXT_CFLAGS += -Wno-tautological-compare -Wno-type-limits -Wno-strict-aliasing
EXT_CFLAGS_PROFILE := $(EXT_CFLAGS) -pg
ifneq ($(USE_CLANG),0)
	EXT_CFLAGS += $(CLANG_EXTRA_FLAGS)
endif

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
CC := $(GCC)
CXX_PROFILE := $(GPP)
CC_PROFILE := $(GCC)
CLANG_CXX := $(CLANGPP_EXE)

ifneq ($(USE_CLANG),0)
	CXX := $(CLANG_CXX)
	CC := $(CLANG_EXE)

	CXXFLAGS_DBG := $(CLANGFLAGS)
	CXXFLAGS_RELEASE := $(CLANGFLAGS_RELEASE)

	BUILD_SYSTEM_DEPS := $(BUILD_SYSTEM_DEPS) $(CLANG_DEPS)
endif

ifeq ($(USE_CCACHE),1)
	CC := ccache $(CC)
	CXX := ccache $(CXX)
	CXX_PROFILE := ccache $(CXX_PROFILE)
	CLANG_CXX := ccache $(CLANG_CXX)
	CXX_ENV += CCACHE_CPP2=yes
	CC_ENV += CCACHE_CPP2=yes
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
CC := $(CC_ENV) $(CC)
CLANG_CXX := $(CXX_ENV) $(CLANG_CXX)
# Not sure if ccache_basedir actually helps at all (I think the generated files make them different?)
LLVM_BUILD_ENV += CCACHE_DIR=$(HOME)/.ccache_llvm CCACHE_BASEDIR=$(LLVM_SRC)

BASE_SRCS := $(wildcard src/codegen/*.cpp) $(wildcard src/asm_writing/*.cpp) $(wildcard src/codegen/irgen/*.cpp) $(wildcard src/codegen/opt/*.cpp) $(wildcard src/analysis/*.cpp) $(wildcard src/core/*.cpp) src/codegen/profiling/profiling.cpp src/codegen/profiling/dumprof.cpp $(wildcard src/runtime/*.cpp) $(wildcard src/runtime/builtin_modules/*.cpp) $(wildcard src/gc/*.cpp) $(wildcard src/capi/*.cpp)
MAIN_SRCS := $(BASE_SRCS) src/jit.cpp
STDLIB_SRCS := $(wildcard src/runtime/inline/*.cpp)
SRCS := $(MAIN_SRCS) $(STDLIB_SRCS)
STDLIB_OBJS := stdlib.bc.o stdlib.stripped.bc.o
STDLIB_RELEASE_OBJS := stdlib.release.bc.o
ASM_SRCS := $(wildcard src/runtime/*.S)

STDMODULE_SRCS := \
	errnomodule.c \
	shamodule.c \
	sha256module.c \
	sha512module.c \
	_math.c \
	mathmodule.c \
	md5.c \
	md5module.c \
	_randommodule.c \
	_sre.c \
	operator.c \
	binascii.c \
	pwdmodule.c \
	posixmodule.c \
	_struct.c \
	datetimemodule.c \
	_functoolsmodule.c \
	_collectionsmodule.c \
	itertoolsmodule.c \
	resource.c \
	signalmodule.c \
	selectmodule.c \
	fcntlmodule.c \
	timemodule.c \
	arraymodule.c \
	zlibmodule.c \
	_codecsmodule.c \
	socketmodule.c \
	unicodedata.c \
	_weakref.c \
	cStringIO.c \
	_io/bufferedio.c \
	_io/bytesio.c \
	_io/fileio.c \
	_io/iobase.c \
	_io/_iomodule.c \
	_io/stringio.c \
	_io/textio.c \
	zipimport.c \
	_csv.c \
	_ssl.c \
	getpath.c \
	_sqlite/cache.c \
	_sqlite/connection.c \
	_sqlite/cursor.c \
	_sqlite/microprotocols.c \
	_sqlite/module.c \
	_sqlite/prepare_protocol.c \
	_sqlite/row.c \
	_sqlite/statement.c \
	_sqlite/util.c \
	$(EXTRA_STDMODULE_SRCS)

STDOBJECT_SRCS := \
	structseq.c \
	capsule.c \
	stringobject.c \
	exceptions.c \
	unicodeobject.c \
	unicodectype.c \
	bytearrayobject.c \
	bytes_methods.c \
	weakrefobject.c \
	memoryobject.c \
	iterobject.c \
	bufferobject.c \
	$(EXTRA_STDOBJECT_SRCS)

STDPYTHON_SRCS := \
	pyctype.c \
	getargs.c \
	formatter_string.c \
	pystrtod.c \
	dtoa.c \
	formatter_unicode.c \
	structmember.c \
	marshal.c \
	$(EXTRA_STDPYTHON_SRCS)

STDPARSER_SRCS := \
	myreadline.c

FROM_CPYTHON_SRCS := $(addprefix from_cpython/Modules/,$(STDMODULE_SRCS)) $(addprefix from_cpython/Objects/,$(STDOBJECT_SRCS)) $(addprefix from_cpython/Python/,$(STDPYTHON_SRCS)) $(addprefix from_cpython/Parser/,$(STDPARSER_SRCS))

# The stdlib objects have slightly longer dependency chains,
# so put them first in the list:
OBJS := $(STDLIB_OBJS) $(SRCS:.cpp=.o) $(FROM_CPYTHON_SRCS:.c=.o) $(ASM_SRCS)
ASTPRINT_OBJS := $(STDLIB_OBJS) $(BASE_SRCS:.cpp=.o) $(FROM_CPYTHON_SRCS:.c=.o) $(ASM_SRCS)
PROFILE_OBJS := $(STDLIB_RELEASE_OBJS) $(MAIN_SRCS:.cpp=.prof.o) $(STDLIB_SRCS:.cpp=.release.o) $(FROM_CPYTHON_SRCS:.c=.prof.o) $(ASM_SRCS)
OPT_OBJS := $(STDLIB_RELEASE_OBJS) $(SRCS:.cpp=.release.o) $(FROM_CPYTHON_SRCS:.c=.release.o) $(ASM_SRCS)

OPTIONAL_SRCS := src/codegen/profiling/oprofile.cpp src/codegen/profiling/pprof.cpp
TOOL_SRCS := $(wildcard $(TOOLS_DIR)/*.cpp)

UNITTEST_DIR := $(TEST_DIR)/unittests
UNITTEST_SRCS := $(wildcard $(UNITTEST_DIR)/*.cpp)

NONSTDLIB_SRCS := $(MAIN_SRCS) $(OPTIONAL_SRCS) $(TOOL_SRCS) $(UNITTEST_SRCS)

.DEFAULT_GOAL := pyston_dbg
# _ :
	# $(MAKE) pyston_dbg || (clear; $(MAKE) pyston_dbg -j1 ERROR_LIMIT=1)

.PHONY: all _all
# all: llvm
	# @# have to do this in a recursive make so that dependency is enforced:
	# $(MAKE) pyston_all
# all: pyston_dbg pyston_release pyston_oprof pyston_prof $(OPTIONAL_SRCS:.cpp=.o) ext_python ext_pyston
all: pyston_dbg pyston_release pyston_prof ext_python ext_pyston unittests

ALL_HEADERS := $(wildcard src/*/*.h) $(wildcard src/*/*/*.h) $(wildcard from_cpython/Include/*.h)
tags: $(SRCS) $(OPTIONAL_SRCS) $(FROM_CPYTHON_SRCS) $(ALL_HEADERS)
	$(ECHO) Calculating tags...
	$(VERB) $(CTAGS) $^

TAGS: $(SRCS) $(OPTIONAL_SRCS) $(FROM_CPYTHON_SRCS) $(ALL_HEADERS)
	$(ECHO) Calculating TAGS...
	$(VERB) $(ETAGS) $^

NON_ENTRY_OBJS := $(filter-out src/jit.o,$(OBJS))

define add_unittest
$(eval \
ifneq ($(USE_CMAKE),1)
$1_unittest: $(GTEST_DIR)/src/gtest-all.o $(NON_ENTRY_OBJS) $(BUILD_SYSTEM_DEPS) $(UNITTEST_DIR)/$1.o
	$(ECHO) Linking $$@
	$(VERB) $(CXX) $(NON_ENTRY_OBJS) $(GTEST_DIR)/src/gtest-all.o $(GTEST_DIR)/src/gtest_main.o $(UNITTEST_DIR)/$1.o $(LDFLAGS) -o $$@
else
.PHONY: $1_unittest
$1_unittest:
	$(NINJA) -C $(HOME)/pyston-build-dbg $1_unittest $(NINJAFLAGS)
	ln -sf $(HOME)/pyston-build-dbg/$1_unittest .
endif
dbg_$1_unittests: $1_unittest
	zsh -c 'ulimit -v $(MAX_MEM_KB); ulimit -d $(MAX_MEM_KB); time $(GDB) $(GDB_CMDS) --args ./$1_unittest --gtest_break_on_failure $(ARGS)'
unittests:: $1_unittest
run_$1_unittests: $1_unittest
	zsh -c 'ulimit -v $(MAX_MEM_KB); ulimit -d $(MAX_MEM_KB); time ./$1_unittest $(ARGS)'
run_unittests:: run_$1_unittests
)
endef

GDB_CMDS := $(GDB_PRE_CMDS) --ex "set confirm off" --ex "handle SIGUSR2 pass nostop noprint" --ex run --ex "bt 20" $(GDB_POST_CMDS)
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
ifneq ($(USE_CMAKE),1)
format:
	cd src && find \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 $(LLVM_BIN)/clang-format -style=file -i
check_format:
	$(ECHO) checking formatting...
	$(VERB) cd src && ../tools/check_format.sh $(LLVM_BIN)/clang-format
else
format:
	$(NINJA) -C $(HOME)/pyston-build-release format
check_format:
	$(NINJA) -C $(HOME)/pyston-build-release check-format
endif

.PHONY: analyze
analyze:
	$(MAKE) clean
	PATH=$$PATH:$(DEPS_DIR)/llvm-trunk/tools/clang/tools/scan-view $(DEPS_DIR)/llvm-trunk/tools/clang/tools/scan-build/scan-build \
		 --use-analyzer $(CLANGPP_EXE) --use-c++ $(CLANGPP_EXE) -V \
		$(MAKE) pyston_dbg USE_DISTCC=0 USE_CCACHE=0

.PHONY: lint cpplint
lint: $(PYTHON_EXE_DEPS)
	$(ECHO) linting...
	$(VERB) cd src && $(PYTHON) ../tools/lint.py
cpplint:
	$(VERB) $(PYTHON) $(TOOLS_DIR)/cpplint.py --filter=-whitespace,-build/header_guard,-build/include_order,-readability/todo $(SRCS)

.PHONY: check quick_check
check:
	@# These are ordered roughly in decreasing order of (chance will expose issue) / (time to run test)
	$(MAKE) lint
	$(MAKE) check_format
	$(MAKE) ext_python ext_pyston pyston_dbg

	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_dbg -j$(TEST_THREADS) -k -a=-S $(TESTS_DIR) $(ARGS)
	@# we pass -I to cpython tests & skip failing ones because they are sloooow otherwise
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_dbg -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing $(TEST_DIR)/cpython $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_dbg -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing -t60 $(TEST_DIR)/integration $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_dbg -j$(TEST_THREADS) -k -a=-n -a=-x -a=-S $(TESTS_DIR) $(ARGS)
	@# skip -O for dbg

	$(MAKE) run_unittests ARGS=

	@# Building in gcc mode is helpful to find various compiler-specific warnings.
	@# We've also discovered UB in our code from running in gcc mode, so try running it as well.
	$(MAKE) pyston_gcc
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_gcc -j$(TEST_THREADS) -k -a=-S $(TESTS_DIR) $(ARGS)

	$(MAKE) pyston_release
	@# It can be useful to test release mode, since it actually exposes different functionality
	@# since we can make different decisions about which internal functions to inline or not.
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_release -j$(TEST_THREADS) -k -a=-S $(TESTS_DIR) $(ARGS)
	@# we pass -I to cpython tests and skip failing ones because they are sloooow otherwise
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_release -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing $(TEST_DIR)/cpython $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_release -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing -t60 $(TEST_DIR)/integration $(ARGS)
	@# skip -n for dbg
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston_release -j$(TEST_THREADS) -k -a=-O -a=-x -a=-S $(TESTS_DIR) $(ARGS)

	echo "All tests passed"

quick_check:
	$(MAKE) pyston_dbg
	$(call checksha,./pyston_dbg -q  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_dbg -qn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_dbg -qO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(MAKE) pyston
	$(call checksha,./pyston -q  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston -qn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston -qO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(MAKE) pyston_prof
	$(call checksha,./pyston_prof -q  $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -qn $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)
	$(call checksha,./pyston_prof -qO $(TESTS_DIR)/raytrace_small.py,0544f4621dd45fe94205219488a2576b84dc044d)

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
# $(CLANGPP_EXE): | $(LLVM_TRUNK)/built_release
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

# Put the llvm_configure line in its own file, so that we can force an llvm reconfigure
# if we change the configuration parameters.
# (All non-llvm build targets get rebuilt if the main Makefile is touched, but that is too
# expensive to do for the llvm ones.)
LLVM_CONFIG_INCL := Makefile.llvmconfig
# Sets LLVM_CONFIGURE_LINE:
include $(LLVM_CONFIG_INCL)
ifeq (,$(LLVM_CONFIGURE_LINE))
$(error "did not set configure line")
endif
ifneq ($(ENABLE_INTEL_JIT_EVENTS),0)
LLVM_CONFIGURE_LINE += --with-intel-jitevents
endif

$(LLVM_CONFIGURATION): $(LLVM_SRC)/configure $(LLVM_CONFIG_INCL) | $(LLVM_SRC)/_patched
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
llvm_test_quick: llvm_quick llvm/quick/tools/opt
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

$(LLVM_SRC)/_patched: $(wildcard ./llvm_patches/*) $(wildcard ./clang_patches/*) $(LLVM_REVISION_FILE)
	$(MAKE) llvm_up
llvm_up:
	python $(TOOLS_DIR)/git_svn_gotorev.py $(LLVM_SRC) $(LLVM_REVISION) ./llvm_patches
	python $(TOOLS_DIR)/git_svn_gotorev.py $(LLVM_SRC)/tools/clang $(LLVM_REVISION) ./clang_patches
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

$(TOOLS_DIR)/astprint: $(TOOLS_DIR)/astprint.cpp $(BUILD_SYSTEM_DEPS) $(LLVM_DEPS) $(ASTPRINT_OBJS)
	$(ECHO) Linking $(TOOLS_DIR)/astprint
	$(VERB) $(CXX) $< -o $@ $(LLVM_LIB_DEPS) $(ASTPRINT_OBJS) $(LDFLAGS) $(STDLIB_SRCS:.cpp=.o) $(CXXFLAGS_DBG)

.PHONY: astprint astcompare

astprint: $(TOOLS_DIR)/astprint

astcompare: astprint
	$(ECHO) Running libpypa vs CPython AST result comparison test
	$(TOOLS_DIR)/astprint_test.sh && echo "Success" || echo "Failure"

## END OF TOOLS


CODEGEN_SRCS := $(wildcard src/codegen/*.cpp) $(wildcard src/codegen/*/*.cpp)

# args: suffix (ex: ".release"), CXXFLAGS
define make_compile_config
$(eval \
$$(NONSTDLIB_SRCS:.cpp=$1.o): CXXFLAGS:=$2
$$(SRCS:.cpp=$1.o.bc): CXXFLAGS:=$2

## Need to set CXXFLAGS so that this rule doesn't inherit the '-include' rule from the
## thing that's calling it.  At the same time, also filter out "-DGITREV=foo".
%$1.h.pch: CXXFLAGS:=$(filter-out -DGITREV%,$(2))
%$1.h.pch: %.h $$(BUILD_SYSTEM_DEPS)
	$$(ECHO) Compiling $$@
	$$(VERB) rm -f $$@-*
	$$(VERB) $$(CLANGPP_EXE) $$(CXXFLAGS) -MMD -MP -MF $$(patsubst %.pch,%.d,$$@) -x c++-header $$< -o $$@
$$(CODEGEN_SRCS:.cpp=$1.o): CXXFLAGS += -include src/codegen/irgen$1.h
$$(CODEGEN_SRCS:.cpp=$1.o): src/codegen/irgen$1.h.pch

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
$(call link,_grwl,stdlib.grwl.bc.o $(SRCS:.cpp=.grwl.o),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))
$(call link,_grwl_dbg,stdlib.grwl_dbg.bc.o $(SRCS:.cpp=.grwl_dbg.o),$(LDFLAGS),$(LLVM_DEPS))
$(call link,_nosync,stdlib.nosync.bc.o $(SRCS:.cpp=.nosync.o),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))
pyston_oprof: $(OPT_OBJS) src/codegen/profiling/oprofile.o $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(OPT_OBJS) src/codegen/profiling/oprofile.o $(LDFLAGS_RELEASE) -lopagent -o $@
pyston_pprof: $(OPT_OBJS) src/codegen/profiling/pprof.release.o $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(OPT_OBJS) src/codegen/profiling/pprof.release.o $(LDFLAGS_RELEASE) -lprofiler -o $@
pyston_prof: $(PROFILE_OBJS) $(LLVM_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(PROFILE_OBJS) $(LDFLAGS) -pg -o $@
pyston_profile: $(PROFILE_OBJS) $(LLVM_PROFILE_DEPS)
	$(ECHO) Linking $@
	$(VERB) $(CXX) $(PROFILE_OBJS) $(LDFLAGS_PROFILE) -o $@

clang_check:
	@clang --version >/dev/null || (echo "clang not available"; false)

cmake_check:
	@cmake --version >/dev/null || (echo "cmake not available"; false)
	@ninja --version >/dev/null || (echo "ninja not available"; false)

ifneq ($(USE_CMAKE),1)
$(call link,_dbg,$(OBJS),$(LDFLAGS),$(LLVM_DEPS))
$(call link,_debug,$(OBJS),$(LDFLAGS_DEBUG),$(LLVM_DEBUG_DEPS))
$(call link,_release,$(OPT_OBJS),$(LDFLAGS_RELEASE),$(LLVM_RELEASE_DEPS))

else
CMAKE_DIR_DBG := $(HOME)/pyston-build-dbg
CMAKE_DIR_RELEASE := $(HOME)/pyston-build-release
CMAKE_SETUP_DBG := $(CMAKE_DIR_DBG)/build.ninja
CMAKE_SETUP_RELEASE := $(CMAKE_DIR_RELEASE)/build.ninja
.PHONY: cmake_check clang_check
$(CMAKE_SETUP_DBG):
	@$(MAKE) cmake_check
	@$(MAKE) clang_check
	@mkdir -p $(CMAKE_DIR_DBG)
	cd $(CMAKE_DIR_DBG); CC='clang' CXX='clang++' cmake -GNinja $(HOME)/pyston -DCMAKE_BUILD_TYPE=Debug
$(CMAKE_SETUP_RELEASE):
	@$(MAKE) cmake_check
	@$(MAKE) clang_check
	@mkdir -p $(CMAKE_DIR_RELEASE)
	cd $(CMAKE_DIR_RELEASE); CC='clang' CXX='clang++' cmake -GNinja $(HOME)/pyston -DCMAKE_BUILD_TYPE=Release

.PHONY: pyston_dbg pyston_release
pyston_dbg: $(CMAKE_SETUP_DBG)
	$(NINJA) -C $(HOME)/pyston-build-dbg pyston copy_stdlib copy_libpyston ext_pyston $(NINJAFLAGS)
	ln -sf $(HOME)/pyston-build-dbg/pyston pyston_dbg
pyston_release: $(CMAKE_SETUP_RELEASE)
	$(NINJA) -C $(HOME)/pyston-build-release pyston copy_stdlib copy_libpyston ext_pyston $(NINJAFLAGS)
	ln -sf $(HOME)/pyston-build-release/pyston pyston_release
endif
CMAKE_DIR_GCC := $(HOME)/pyston-build-gcc
CMAKE_SETUP_GCC := $(CMAKE_DIR_GCC)/build.ninja
$(CMAKE_SETUP_GCC):
	@$(MAKE) cmake_check
	@mkdir -p $(CMAKE_DIR_GCC)
	cd $(CMAKE_DIR_GCC); CC='$(GCC)' CXX='$(GPP)' cmake -GNinja $(HOME)/pyston -DCMAKE_BUILD_TYPE=Debug
.PHONY: pyston_gcc
pyston_gcc: $(CMAKE_SETUP_GCC)
	$(NINJA) -C $(HOME)/pyston-build-gcc pyston copy_stdlib copy_libpyston ext_pyston $(NINJAFLAGS)
	ln -sf $(HOME)/pyston-build-gcc/pyston pyston_gcc

-include $(wildcard src/*.d) $(wildcard src/*/*.d) $(wildcard src/*/*/*.d) $(wildcard $(UNITTEST_DIR)/*.d) $(wildcard from_cpython/*/*.d) $(wildcard from_cpython/*/*/*.d)

.PHONY: clean
clean:
	@ find src $(TOOLS_DIR) $(TEST_DIR) ./from_cpython/Modules \( -name '*.o' -o -name '*.d' -o -name '*.py_cache' -o -name '*.bc' -o -name '*.o.ll' -o -name '*.pub.ll' -o -name '*.cache' -o -name 'stdlib*.ll' -o -name '*.pyc' -o -name '*.so' -o -name '*.a' -o -name '*.expected_cache' -o -name '*.pch' \) -print -delete
	@ find \( -name 'pyston*' -executable -type f \) -print -delete
	@ find $(TOOLS_DIR) -maxdepth 0 -executable -type f -print -delete
	@ rm -rf oprofile_data
	@ rm -f *_unittest

# A helper function that lets me run subdirectory rules from the top level;
# ex instead of saying "make tests/run_1", I can just write "make run_1"
#
# The target to ultimately be called must be prefixed with nosearch_, for example:
#     nosearch_example_%: %.py
#         echo $^
#     $(call make_search,example_%)
# This prevents us from searching recursively, which can result in a combinatorial explosion.
define make_search
$(eval \
.PHONY: $1 nosearch_$1
$1: nosearch_$1
$1: $(TESTS_DIR)/nosearch_$1 ;
$1: $(TEST_DIR)/cpython/nosearch_$1 ;
$1: $(TEST_DIR)/integration/nosearch_$1 ;
$1: ./microbenchmarks/nosearch_$1 ;
$1: ./minibenchmarks/nosearch_$1 ;
$1: ./benchmarks/nosearch_$1 ;
$1: $(HOME)/pyston-perf/benchmarking/benchmark_suite/nosearch_$1 ;
$(patsubst %, $$1: %/nosearch_$$1 ;,$(EXTRA_SEARCH_DIRS))
)
endef

RUN_DEPS := ext_pyston

define make_target
$(eval \
.PHONY: test$1 check$1
check$1 test$1: $(PYTHON_EXE_DEPS) pyston$1 ext_pyston
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-S -k $(TESTS_DIR) $(ARGS)
	@# we pass -I to cpython tests and skip failing ones because they are sloooow otherwise
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-S -k --exit-code-only --skip-failing $(TEST_DIR)/cpython $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing -t=60 $(TEST_DIR)/integration $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -a=-x -R pyston$1 -j$(TEST_THREADS) -a=-n -a=-S -k $(TESTS_DIR) $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-O -a=-S -k $(TESTS_DIR) $(ARGS)

.PHONY: run$1 dbg$1
run$1: pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension ./pyston$1 $$(ARGS)
dbg$1: pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension zsh -c 'ulimit -v $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS)'
nosearch_run$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) PYTHONPATH=test/test_extension zsh -c 'ulimit -v $$(MAX_MEM_KB); ulimit -d $$(MAX_MEM_KB); time ./pyston$1 $$(ARGS) $$<'
$$(call make_search,run$1_%)
nosearch_dbg$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) PYTHONPATH=test/test_extension zsh -c 'ulimit -v $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS) $$<'
$$(call make_search,dbg$1_%)

ifneq ($$(ENABLE_VALGRIND),0)
nosearch_memcheck$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension $$(VALGRIND) --tool=memcheck --leak-check=no --db-attach=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memcheck$1_%)
nosearch_memcheck_gdb$1_%: %.py pyston$1 $$(RUN_DEPS)
	set +e; PYTHONPATH=test/test_extension $$(VALGRIND) -v -v -v -v -v --tool=memcheck --leak-check=no --track-origins=yes --vgdb=yes --vgdb-error=0 ./pyston$1 $$(ARGS) $$< & export PID=$$$$! ; \
	$$(GDB) --ex "set confirm off" --ex "target remote | $$(DEPS_DIR)/valgrind-3.10.0-install/bin/vgdb" --ex "continue" --ex "bt" ./pyston$1; kill -9 $$$$PID
$$(call make_search,memcheck_gdb$1_%)
nosearch_memleaks$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension $$(VALGRIND) --tool=memcheck --leak-check=full --leak-resolution=low --show-reachable=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memleaks$1_%)
nosearch_cachegrind$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension $$(VALGRIND) --tool=cachegrind ./pyston$1 $$(ARGS) $$<
$$(call make_search,cachegrind$1_%)
endif

.PHONY: perf$1_%
nosearch_perf$1_%: %.py pyston$1
	PYTHONPATH=test/test_extension perf record -g -- ./pyston$1 -q -p $$(ARGS) $$<
	@$(MAKE) perf_report
$$(call make_search,perf$1_%)

)
endef

.PHONY: perf_report
perf_report:
	perf report -v -n -g flat,1000 | bash $(TOOLS_DIR)/cumulate.sh | less -S

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
$(call make_target,_debug)
$(call make_target,_release)
# $(call make_target,_grwl)
# $(call make_target,_grwl_dbg)
# $(call make_target,_nosync)
$(call make_target,_prof)
$(call make_target,_gcc)

nosearch_runpy_% nosearch_pyrun_%: %.py ext_python
	$(VERB) PYTHONPATH=test/test_extension/build/lib.linux-x86_64-2.7 zsh -c 'time python $<'
$(call make_search,runpy_%)
$(call make_search,pyrun_%)

nosearch_check_%: %.py ext_python ext_pyston
	$(MAKE) check_dbg ARGS="$(patsubst %.py,%,$(notdir $<)) -K"
$(call make_search,check_%)

nosearch_dbgpy_% nosearch_pydbg_%: %.py ext_pythondbg
	export PYTHON_VERSION=$$(python2.7-dbg -V 2>&1 | awk '{print $$2}'); PYTHONPATH=test/test_extension/build/lib.linux-x86_64-2.7-pydebug $(GDB) --ex "dir $(DEPS_DIR)/python-src/python2.7-$$PYTHON_VERSION/debian" $(GDB_CMDS) --args python2.7-dbg $<
$(call make_search,dbgpy_%)
$(call make_search,pydbg_%)

# "kill valgrind":
kv:
	ps aux | awk '/[v]algrind/ {print $$2}' | xargs kill -9; true

# gprof-based profiling:
nosearch_prof_%: %.py pyston_prof
	zsh -c 'time ./pyston_prof $(ARGS) $<'
	gprof ./pyston_prof gmon.out > $(patsubst %,%.out,$@)
$(call make_search,prof_%)
nosearch_profile_%: %.py pyston_profile
	time ./pyston_profile -p $(ARGS) $<
	gprof ./pyston_profile gmon.out > $(patsubst %,%.out,$@)
$(call make_search,profile_%)

# pprof-based profiling:
nosearch_pprof_%: %.py $(PYTHON_EXE_DEPS) pyston_pprof
	CPUPROFILE_FREQUENCY=1000 CPUPROFILE=$@.out ./pyston_pprof -p $(ARGS) $<
	pprof --raw pyston_pprof $@.out > $@_raw.out
	$(PYTHON) codegen/profiling/process_pprof.py $@_raw.out pprof.jit > $@_processed.out
	pprof --text $@_processed.out
	# rm -f pprof.out pprof.raw pprof.jit
$(call make_search,pprof_%)

# oprofile-based profiling:
.PHONY: oprof_collect_% opreport
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
nosearch_oprof_%: oprof_collect_%
	$(MAKE) opreport
$(call make_search,oprof_%)
opreport:
	! [ -d oprofile_data ]
	opreport -l -t 0.2 -a pyston_oprof
	# opreport lib-image:pyston_oprof -l -t 0.2 -a | head -n 25

.PHONY: oprof_collectcg_% opreportcg
oprof_collectcg_%: %.py pyston_oprof
	operf -g -e CPU_CLK_UNHALTED:90000 ./pyston_oprof -p $(ARGS) $<
nosearch_oprofcg_%: oprof_collectcg_%
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
		Makefile $$(find . \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) ); do clear; $(MAKE) $$TARGET $(WATCH_ARGS); done )
		# Makefile $$(find \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) -o -type d ); do clear; $(MAKE) $(patsubst watch_%,%,$@); done )
		# -r . ; do clear; $(MAKE) $(patsubst watch_%,%,$@); done
watch: watch_pyston_dbg
watch_vim:
	$(MAKE) watch WATCH_ARGS='COLOR=0 USE_DISTCC=0 -j1 2>&1 | tee compile.log'
wdbg_%:
	$(MAKE) $(patsubst wdbg_%,watch_dbg_%,$@) GDB_POST_CMDS="--ex quit"

.PHONY: test_asm test_cpp_asm
test_asm:
	$(CLANGPP_EXE) $(TEST_DIR)/test.s -c -o test_asm
	objdump -d test_asm | less
	@ rm test_asm
test_cpp_asm:
	$(CLANGPP_EXE) $(TEST_DIR)/test.cpp -o test_asm -c -O3 -std=c++11
	# $(GPP) tests/test.cpp -o test_asm -c -O3
	objdump -d test_asm | less
	rm test_asm
test_cpp_dwarf:
	$(CLANGPP_EXE) $(TEST_DIR)/test.cpp -o test_asm -c -O3 -std=c++11 -g
	# $(GPP) tests/test.cpp -o test_asm -c -O3
	objdump -W test_asm | less
	rm test_asm
test_cpp_ll:
	$(CLANGPP_EXE) $(TEST_DIR)/test.cpp -o test.ll -c -O3 -emit-llvm -S -std=c++11 -g
	less test.ll
	rm test.ll
.PHONY: bench_exceptions
bench_exceptions:
	$(CLANGPP_EXE) $(TEST_DIR)/bench_exceptions.cpp -o bench_exceptions -O3 -std=c++11
	zsh -c 'ulimit -v $(MAX_MEM_KB); ulimit -d $(MAX_MEM_KB); time ./bench_exceptions'
	rm bench_exceptions

TEST_EXT_MODULE_NAMES := basic_test descr_test slots_test

.PHONY: ext_pyston
ext_pyston: $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/%.pyston.so)
ifneq ($(SELF_HOST),1)
$(TEST_DIR)/test_extension/%.pyston.so: $(TEST_DIR)/test_extension/%.o
	$(CC) -pthread -shared -Wl,-O1 -Wl,-Bsymbolic-functions -Wl,-z,relro $< -o $@ -g
$(TEST_DIR)/test_extension/%.o: $(TEST_DIR)/test_extension/%.c $(wildcard from_cpython/Include/*.h)
	$(CC) -pthread $(EXT_CFLAGS) -c $< -o $@
else
# Hax: we want to generate multiple targets from a single rule, and run the rule only if the
# dependencies have been updated, and only run it once for all the targets.
# So just tell make to generate the first extension module, and that the non-first ones just
# depend on the first one.
$(TEST_DIR)/test_extension/$(firstword $(TEST_EXT_MODULE_NAMES)).pyston.so: $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/%.c) | pyston_dbg
	$(MAKE) ext_pyston_selfhost
NONFIRST_EXT := $(wordlist 2,9999,$(TEST_EXT_MODULE_NAMES))
$(NONFIRST_EXT:%=$(TEST_DIR)/test_extension/%.pyston.so): $(TEST_DIR)/test_extension/$(firstword $(TEST_EXT_MODULE_NAMES)).pyston.so
endif

.PHONY: ext_pyston_selfhost dbg_ext_pyston_selfhost ext_pyston_selfhost_release
ext_pyston_selfhost: pyston_dbg $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/*.c)
	cd $(TEST_DIR)/test_extension; DISTUTILS_DEBUG=1 time ../../pyston_dbg setup.py build
	cd $(TEST_DIR)/test_extension; ln -sf $(TEST_EXT_MODULE_NAMES:%=build/lib.unknown-2.7/%.pyston.so) .
dbg_ext_pyston_selfhost: pyston_dbg $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/*.c)
	cd $(TEST_DIR)/test_extension; DISTUTILS_DEBUG=1 $(GDB) $(GDB_CMDS) --args ../../pyston_dbg setup.py build
	cd $(TEST_DIR)/test_extension; ln -sf $(TEST_EXT_MODULE_NAMES:%=build/lib.unknown-2.7/%.pyston.so) .
ext_pyston_selfhost_release: pyston_release $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/*.c)
	cd $(TEST_DIR)/test_extension; DISTUTILS_DEBUG=1 time ../../pyston_release setup.py build
	cd $(TEST_DIR)/test_extension; ln -sf $(TEST_EXT_MODULE_NAMES:%=build/lib.unknown-2.7/%.pyston.so) .

.PHONY: ext_python ext_pythondbg
ext_python: $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/*.c)
	cd $(TEST_DIR)/test_extension; python setup.py build
ext_pythondbg: $(TEST_EXT_MODULE_NAMES:%=$(TEST_DIR)/test_extension/*.c)
	cd $(TEST_DIR)/test_extension; python2.7-dbg setup.py build

$(FROM_CPYTHON_SRCS:.c=.o): %.o: %.c $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling C file to $@
	$(VERB) $(CC) $(EXT_CFLAGS) -c $< -o $@ -g -MMD -MP -MF $(patsubst %.o,%.d,$@) -O0

$(FROM_CPYTHON_SRCS:.c=.o.ll): %.o.ll: %.c $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling C file to $@
	$(VERB) $(CLANG_EXE) $(EXT_CFLAGS) -S -emit-llvm -c $< -o $@ -g -MMD -MP -MF $(patsubst %.o,%.d,$@) -O3 -g0

$(FROM_CPYTHON_SRCS:.c=.release.o): %.release.o: %.c $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling C file to $@
	$(VERB) $(CC) $(EXT_CFLAGS) -c $< -o $@ -g -MMD -MP -MF $(patsubst %.o,%.d,$@)

$(FROM_CPYTHON_SRCS:.c=.prof.o): %.prof.o: %.c $(BUILD_SYSTEM_DEPS)
	$(ECHO) Compiling C file to $@
	$(VERB) $(CC_PROFILE) $(EXT_CFLAGS_PROFILE) -c $< -o $@ -g -MMD -MP -MF $(patsubst %.o,%.d,$@)




# TESTING:

_plugins/clang_capi.so: plugins/clang_capi.cpp $(BUILD_SYSTEM_DEPS)
	@# $(CXX) $< -o $@ -c -I/usr/lib/llvm-3.5/include -std=c++11 -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS
	$(CXX) $< -o $@ -std=c++11 $(LLVM_CXXFLAGS) -I$(LLVM_SRC)/tools/clang/include -I$(LLVM_BUILD)/tools/clang/include -shared

plugins/clang_capi.o: plugins/clang_capi.cpp $(BUILD_SYSTEM_DEPS)
	@# $(CXX) $< -o $@ -c -I/usr/lib/llvm-3.5/include -std=c++11 -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS
	$(CXX) $< -o $@ -std=c++11 $(LLVM_CXXFLAGS) -O0 -I$(LLVM_SRC)/tools/clang/include -I$(LLVM_BUILD)/tools/clang/include -c
plugins/clang_capi: plugins/clang_capi.o $(BUILD_SYSTEM_DEPS)
	@# $(CXX) $< -o $@ -c -I/usr/lib/llvm-3.5/include -std=c++11 -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS
	$(CXX) $< -o $@ -L$(LLVM_BUILD)/Release/lib -lclangASTMatchers -lclangRewrite -lclangFrontend -lclangDriver -lclangTooling -lclangParse -lclangSema -lclangAnalysis -lclangAST -lclangEdit -lclangLex -lclangBasic -lclangSerialization $(shell $(LLVM_BUILD)/Release+Asserts/bin/llvm-config --ldflags --system-libs --libs all)
plugins/clang_capi.so: plugins/clang_capi.o $(BUILD_SYSTEM_DEPS)
	@# $(CXX) $< -o $@ -c -I/usr/lib/llvm-3.5/include -std=c++11 -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS
	$(CXX) $< -o $@ -shared
.PHONY: plugin_test
plugin_test: plugins/clang_capi.so
	$(CLANG_CXX) -Xclang -load -Xclang plugins/clang_capi.so -Xclang -add-plugin -Xclang print-fns test/test.cpp -c -S -o -
.PHONY: tool_test
tool_test: plugins/clang_capi
	plugins/clang_capi test/test.cpp --
