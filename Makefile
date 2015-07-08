SHELL := /bin/bash

# prints variables for debugging
print-%: ; @echo $($*)

# Disable builtin rules:
.SUFFIXES:

DEPS_DIR := $(HOME)/pyston_deps

SRC_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(SRC_DIR)/build

USE_CLANG  := 1
USE_CCACHE := 1
USE_DISTCC := 0

PYPY := pypy

ENABLE_VALGRIND := 0

GDB := gdb
# If you followed the old install instructions:
# GCC_DIR := $(DEPS_DIR)/gcc-4.8.2-install
GCC_DIR := /usr

USE_DEBUG_LIBUNWIND := 0

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

NINJA := ninja

CMAKE_DIR_DBG := $(BUILD_DIR)/Debug
CMAKE_DIR_DBG_GCC := $(BUILD_DIR)/Debug-gcc
CMAKE_DIR_RELEASE := $(BUILD_DIR)/Release
CMAKE_SETUP_DBG := $(CMAKE_DIR_DBG)/build.ninja
CMAKE_SETUP_RELEASE := $(CMAKE_DIR_RELEASE)/build.ninja

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

CLANG_EXE := $(LLVM_BIN)/clang
CLANGPP_EXE := $(LLVM_BIN)/clang++

ifeq ($(ENABLE_VALGRIND),0)
	CMAKE_VALGRIND := 
else
	CMAKE_VALGRIND := -DENABLE_VALGRIND=ON -DVALGRIND_DIR=$(DEPS_DIR)/valgrind-3.10.0-install/
endif


# We get multiple shared libraries (libstdc++, libgcc_s) from the gcc installation:
ifneq ($(GCC_DIR),/usr)
	COMMON_LDFLAGS += -Wl,-rpath $(GCC_DIR)/lib64
endif

BUILD_SYSTEM_DEPS := Makefile Makefile.local $(wildcard build_system/*)

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
endif

CXX := $(CXX_ENV) $(CXX)
CXX_PROFILE := $(CXX_ENV) $(CXX_PROFILE)
CC := $(CC_ENV) $(CC)
CLANG_CXX := $(CXX_ENV) $(CLANG_CXX)

.DEFAULT_GOAL := pyston_dbg

# The set of dependencies (beyond the executable) required to do `make check` / `make check_foo`.
# The tester bases all paths based on the executable, so in cmake mode we need to have cmake
# build all of the shared modules.
check-deps:
	$(NINJA) -C $(CMAKE_DIR_DBG) check-deps

.PHONY: all _all
# all: llvm
	# @# have to do this in a recursive make so that dependency is enforced:
	# $(MAKE) pyston_all
# all: pyston_dbg pyston_release pyston_oprof pyston_prof $(OPTIONAL_SRCS:.cpp=.o) ext_python ext_pyston
all: pyston_dbg pyston_release pyston_gcc unittests check-deps

define add_unittest
$(eval \
.PHONY: $1_unittest
$1_unittest:
	$(NINJA) -C $(CMAKE_DIR_DBG) $1_unittest $(NINJAFLAGS)
	ln -sf $(CMAKE_DIR_DBG)/$1_unittest .
dbg_$1_unittests: $1_unittest
	zsh -c 'ulimit -m $(MAX_MEM_KB); time $(GDB) $(GDB_CMDS) --args ./$1_unittest --gtest_break_on_failure $(ARGS)'
unittests:: $1_unittest
run_$1_unittests: $1_unittest
	zsh -c 'ulimit -m $(MAX_MEM_KB); time ./$1_unittest $(ARGS)'
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

.PHONY: check
check:
	@# These are ordered roughly in decreasing order of (chance will expose issue) / (time to run test)

	$(MAKE) pyston_dbg check-deps
	( cd $(CMAKE_DIR_DBG) && ctest -V )

	$(MAKE) pyston_release
	( cd $(CMAKE_DIR_RELEASE) && ctest -V -R pyston )

	echo "All tests passed"

# A stripped down set of tests, meant as a quick smoke test to run before submitting a PR and having
# Travis-CI do the full test.
.PHONY: quick_check
quick_check:
	$(MAKE) pyston_dbg
	$(MAKE) check-deps
	( cd $(CMAKE_DIR_DBG) && ctest -V -R 'check-format|unittests|pyston_defaults_tests|pyston_defaults_cpython' )

## TOOLS:

.PHONY: astprint astcompare

astprint: $(TOOLS_DIR)/astprint

astcompare: astprint
	$(ECHO) Running libpypa vs CPython AST result comparison test
	$(TOOLS_DIR)/astprint_test.sh && echo "Success" || echo "Failure"

## END OF TOOLS

%.o: %.cpp $(CMAKE_SETUP_DBG)
	$(NINJA) -C $(CMAKE_DIR_DBG) src/CMakeFiles/PYSTON_OBJECTS.dir/$(patsubst src/%.o,%.cpp.o,$@) $(NINJAFLAGS)
%.release.o: %.cpp $(CMAKE_SETUP_RELEASE)
	$(NINJA) -C $(CMAKE_DIR_RELEASE) src/CMakeFiles/PYSTON_OBJECTS.dir/$(patsubst src/%.release.o,%.cpp.o,$@) $(NINJAFLAGS)

clang_check:
	@clang --version >/dev/null || (echo "clang not available"; false)

cmake_check:
	@cmake --version >/dev/null || (echo "cmake not available"; false)
	@ninja --version >/dev/null || (echo "ninja not available"; false)

.PHONY: cmake_check clang_check
$(CMAKE_SETUP_DBG):
	@$(MAKE) cmake_check
	@$(MAKE) clang_check
	@mkdir -p $(CMAKE_DIR_DBG)
	cd $(CMAKE_DIR_DBG); CC='clang' CXX='clang++' cmake -GNinja $(SRC_DIR) -DTEST_THREADS=$(TEST_THREADS) -DCMAKE_BUILD_TYPE=Debug $(CMAKE_VALGRIND)
$(CMAKE_SETUP_RELEASE):
	@$(MAKE) cmake_check
	@$(MAKE) clang_check
	@mkdir -p $(CMAKE_DIR_RELEASE)
	cd $(CMAKE_DIR_RELEASE); CC='clang' CXX='clang++' cmake -GNinja $(SRC_DIR) -DTEST_THREADS=$(TEST_THREADS) -DCMAKE_BUILD_TYPE=Release

# Shared modules (ie extension modules that get built using pyston on setup.py) that we will ask CMake
# to build.  You can flip this off to allow builds to continue even if self-hosting the sharedmods would fail.
CMAKE_SHAREDMODS := sharedmods ext_pyston

.PHONY: pyston_dbg pyston_release
pyston_dbg: $(CMAKE_SETUP_DBG)
	$(NINJA) -C $(CMAKE_DIR_DBG) pyston copy_stdlib copy_libpyston $(CMAKE_SHAREDMODS) ext_cpython $(NINJAFLAGS)
	ln -sf $(CMAKE_DIR_DBG)/pyston pyston_dbg
pyston_release: $(CMAKE_SETUP_RELEASE)
	$(NINJA) -C $(CMAKE_DIR_RELEASE) pyston copy_stdlib copy_libpyston $(CMAKE_SHAREDMODS) ext_cpython $(NINJAFLAGS)
	ln -sf $(CMAKE_DIR_RELEASE)/pyston pyston_release

CMAKE_DIR_GCC := $(CMAKE_DIR_DBG_GCC)
CMAKE_SETUP_GCC := $(CMAKE_DIR_GCC)/build.ninja
$(CMAKE_SETUP_GCC):
	@$(MAKE) cmake_check
	@mkdir -p $(CMAKE_DIR_GCC)
	cd $(CMAKE_DIR_GCC); CC='$(GCC)' CXX='$(GPP)' cmake -GNinja $(SRC_DIR) -DCMAKE_BUILD_TYPE=Debug $(CMAKE_VALGRIND)
.PHONY: pyston_gcc
pyston_gcc: $(CMAKE_SETUP_GCC)
	$(NINJA) -C $(CMAKE_DIR_DBG_GCC) pyston copy_stdlib copy_libpyston sharedmods ext_pyston ext_cpython $(NINJAFLAGS)
	ln -sf $(CMAKE_DIR_DBG_GCC)/pyston pyston_gcc

.PHONY: format check_format
format: $(CMAKE_SETUP_RELEASE)
	$(NINJA) -C $(CMAKE_DIR_RELEASE) format
check_format: $(CMAKE_SETUP_RELEASE)
	$(NINJA) -C $(CMAKE_DIR_RELEASE) check-format

.PHONY: clean
clean:
	@ find src $(TOOLS_DIR) $(TEST_DIR) ./from_cpython ./lib_pyston \( -name '*.o' -o -name '*.d' -o -name '*.py_cache' -o -name '*.bc' -o -name '*.o.ll' -o -name '*.pub.ll' -o -name '*.cache' -o -name 'stdlib*.ll' -o -name '*.pyc' -o -name '*.so' -o -name '*.a' -o -name '*.expected_cache' -o -name '*.pch' \) -print -delete
	@ find \( -name 'pyston*' -executable -type f \) -print -delete
	@ rm -vf pyston_dbg pyston_release pyston_gcc
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
$1: $(TEST_DIR)/extra/nosearch_$1 ;
$1: ./microbenchmarks/nosearch_$1 ;
$1: ./minibenchmarks/nosearch_$1 ;
$1: ./benchmarks/nosearch_$1 ;
$1: $(HOME)/pyston-perf/benchmarking/benchmark_suite/nosearch_$1 ;
$(patsubst %, $$1: %/nosearch_$$1 ;,$(EXTRA_SEARCH_DIRS))
)
endef

define make_target
$(eval \
.PHONY: test$1 check$1
check$1 test$1: $(PYTHON_EXE_DEPS) pyston$1
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-S -k $(TESTS_DIR) $(ARGS)
	@# we pass -I to cpython tests and skip failing ones because they are sloooow otherwise
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-S -k --exit-code-only --skip-failing -t30 $(TEST_DIR)/cpython $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -k -a=-S --exit-code-only --skip-failing -t600 $(TEST_DIR)/integration $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -a=-x -R pyston$1 -j$(TEST_THREADS) -a=-n -a=-S -k $(TESTS_DIR) $(ARGS)
	$(PYTHON) $(TOOLS_DIR)/tester.py -R pyston$1 -j$(TEST_THREADS) -a=-O -a=-S -k $(TESTS_DIR) $(ARGS)

.PHONY: run$1 dbg$1
run$1: pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension:$${PYTHONPATH} ./pyston$1 $$(ARGS)
dbg$1: pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension:$${PYTHONPATH} zsh -c 'ulimit -m $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS)'
nosearch_run$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) PYTHONPATH=test/test_extension:$${PYTHONPATH} zsh -c 'ulimit -m $$(MAX_MEM_KB); time ./pyston$1 $$(ARGS) $$<'
$$(call make_search,run$1_%)
nosearch_dbg$1_%: %.py pyston$1 $$(RUN_DEPS)
	$(VERB) PYTHONPATH=test/test_extension:$${PYTHONPATH} zsh -c 'ulimit -m $$(MAX_DBG_MEM_KB); $$(GDB) $$(GDB_CMDS) --args ./pyston$1 $$(ARGS) $$<'
$$(call make_search,dbg$1_%)

ifneq ($$(ENABLE_VALGRIND),0)
nosearch_memcheck$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension:$${PYTHONPATH} $$(VALGRIND) --tool=memcheck --leak-check=no --db-attach=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memcheck$1_%)
nosearch_memcheck_gdb$1_%: %.py pyston$1 $$(RUN_DEPS)
	set +e; PYTHONPATH=test/test_extension:$${PYTHONPATH} $$(VALGRIND) -v -v -v -v -v --tool=memcheck --leak-check=no --track-origins=yes --vgdb=yes --vgdb-error=0 ./pyston$1 $$(ARGS) $$< & export PID=$$$$! ; \
	$$(GDB) --ex "set confirm off" --ex "target remote | $$(DEPS_DIR)/valgrind-3.10.0-install/bin/vgdb" --ex "continue" --ex "bt" ./pyston$1; kill -9 $$$$PID
$$(call make_search,memcheck_gdb$1_%)
nosearch_memleaks$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension:$${PYTHONPATH} $$(VALGRIND) --tool=memcheck --leak-check=full --leak-resolution=low --show-reachable=yes ./pyston$1 $$(ARGS) $$<
$$(call make_search,memleaks$1_%)
nosearch_cachegrind$1_%: %.py pyston$1 $$(RUN_DEPS)
	PYTHONPATH=test/test_extension:$${PYTHONPATH} $$(VALGRIND) --tool=cachegrind ./pyston$1 $$(ARGS) $$<
$$(call make_search,cachegrind$1_%)
endif

.PHONY: perf$1_%
nosearch_perf$1_%: %.py pyston$1
	PYTHONPATH=test/test_extension:$${PYTHONPATH} perf record -g -- ./pyston$1 -q -p $$(ARGS) $$<
	@$(MAKE) perf_report
$$(call make_search,perf$1_%)

)
endef

.PHONY: perf_report
perf_report:
	perf report -n

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
nosearch_pypyrun_%: %.py ext_python
	$(VERB) PYTHONPATH=test/test_extension/build/lib.linux-x86_64-2.7 zsh -c 'time $(PYPY) $<'
$(call make_search,runpy_%)
$(call make_search,pyrun_%)
$(call make_search,pypyrun_%)

nosearch_check_%: %.py pyston_dbg check-deps
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
	@ ( ulimit -t 60; ulimit -m $(MAK_MEM_KB); \
		TARGET=$(dir $@)$(patsubst watch_%,%,$(notdir $@)); \
		clear; $(MAKE) $$TARGET $(WATCH_ARGS); true; \
		while inotifywait -q -e modify -e attrib -e move -e move_self -e create -e delete -e delete_self \
		Makefile $$(find src test \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) ); do clear; \
			$(MAKE) $$TARGET $(WATCH_ARGS); \
		done )
		# Makefile $$(find \( -name '*.cpp' -o -name '*.h' -o -name '*.py' \) -o -type d ); do clear; $(MAKE) $(patsubst watch_%,%,$@); done )
		# -r . ; do clear; $(MAKE) $(patsubst watch_%,%,$@); done
watch: watch_pyston_dbg
watch_vim:
	$(MAKE) watch WATCH_ARGS='COLOR=0 USE_DISTCC=0 -j1 2>&1 | tee compile.log'
wdbg_%:
	$(MAKE) $(patsubst wdbg_%,watch_dbg_%,$@) GDB_POST_CMDS="--ex quit"

.PHONY: head_%
HEAD := 40
head_%:
	@ bash -c "set -o pipefail; script -e -q -c '$(MAKE) $(dir $@)$(patsubst head_%,%,$(notdir $@))' /dev/null | head -n$(HEAD)"
head: head_pyston_dbg
.PHONY: hwatch_%
hwatch_%:
	@ $(MAKE) $(dir $@)$(patsubst hwatch_%,watch_head_%,$(notdir $@))
hwatch: hwatch_pyston_dbg

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
	zsh -c 'ulimit -m $(MAX_MEM_KB); time ./bench_exceptions'
	rm bench_exceptions

