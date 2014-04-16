Pyston currently only supports installing from source; the following instructions have only been tested on Ubuntu, but should ideally work on a Mac as well.

Pyston expects to find all of its dependencies in ~/pyston_deps:
```
mkdir ~/pyston_deps
```

### Compiler for clang

clang requires a fairly modern [host compiler](http://llvm.org/docs/GettingStarted.html#host-c-toolchain-both-compiler-and-standard-library), so typically you will have to install a new one.  The easiest thing to do is to just create a fresh build of GCC:

```
sudo apt-get install libgmp-dev libmpfr-dev libmpc-dev make build-essential libtool zip gcc-multilib autogen
cd ~/pyston_deps
wget 'http://www.netgull.com/gcc/releases/gcc-4.8.2/gcc-4.8.2.tar.bz2'
tar xvf gcc-4.8.2.tar.bz2
mkdir gcc-4.8.2-{build,install}
cd gcc-4.8.2-build
# Space- and time-saving configuration:
../gcc-4.8.2/configure --disable-bootstrap --enable-languages=c,c++ --prefix=$HOME/pyston_deps/gcc-4.8.2-install
# full configuration:
# ../gcc-4.8.2/configure --prefix=$HOME/pyston_deps/gcc-4.8.2-install
make -j4
make check
make install
```

### ccache

ccache is a build tool that can help speed up redundant compilations.  It's not strictly necessary but it's useful enough to be enabled by default; you can disable it by adding `USE_CCACHE := 0` to your Makefile.local.  To get it, run:
```
sudo apt-get install ccache
```

### LLVM dependencies
```
sudo apt-get install libncurses5-dev zlib1g-dev
```

### LLVM + clang

LLVM and clang depend on a pretty modern compiler; the steps below assume you uinstalled GCC 4.8.2 as described above.  It should be possible to build using clang >= 3.1, such as what you might find on a Mac, but that will require changes to the way LLVM is configured (specified in src/Makefile) that I haven't tested.

```
cd ~/pyston_deps
git clone http://llvm.org/git/llvm.git llvm-trunk
git clone http://llvm.org/git/clang.git llvm-trunk/tools/clang
cd ~/pyston/src
make llvm_up
make llvm_configure
make llvm -j4
```

There seem to be some lingering issues with the LLVM build that haven't been identified yet; if the last step fails with errors along the lines of "rm: could not find file foo.tmp", it is quite likely that simply running it again will cause it to continue successfully.  You may have to do this multiple times, unfortunately.

### libunwind

```
cd ~/pyston_deps
wget http://download.savannah.gnu.org/releases/libunwind/libunwind-1.1.tar.gz
tar xvf libunwind-1.1.tar.gz
mkdir libunwind-1.1-install
cd libunwind-1.1
# disable shared libraries because we'll be installing this in a place that the loader can't find it.
./configure --prefix=$HOME/pyston_deps/libunwind-1.1-install --enable-shared=0
make -j4
make install
```

TODO would be nice to install this locally like the rest of the dependencies

### valgrind

valgrind is close to being an optional dependency, but since Pyston contains a custom memory allocator, it has some basic (and mostly-broken) valgrind hooks to let it know what memory is safe to access or not.  TODO it'd be nice to be able to turn that off with a Makefile flag since it's not useful for most people.

You may be able to install valgrind from your system package manager (`apt-get install valgrind`), but it is likely to be an old enough version that it doesn't support some newer instructions and may crash when running.  The safest thing to do is to do a full installation from source:

```
cd ~/pyston_deps
wget http://valgrind.org/downloads/valgrind-3.9.0.tar.bz2
tar xvf valgrind-3.9.0.tar.bz2
mkdir valgrind-3.9.0-install
cd valgrind-3.9.0
./configure --prefix=$HOME/pyston_deps/valgrind-3.9.0-install
make -j4
make install
sudo apt-get install libc6-dbg
```

Then, add this line to your Makefile.local:
```
VALGRIND := VALGRIND_LIB=$(HOME)/pyston_deps/valgrind-3.9.0-install/lib/valgrind $(HOME)/pyston_deps/valgrind-3.9.0-install/bin/valgrind
```

# Optional dependencies

### distcc
```
sudo apt-get install distcc distcc-pump
```

You can then use distcc by doing `make USE_DISTCC=1`

### gtest

For running the unittests, though all the unittests are currently disabled:

```
cd ~/pyston_deps
wget https://googletest.googlecode.com/files/gtest-1.7.0.zip
unzip gtest-1.7.0.zip
cd gtest-1.7.0
./configure CXXFLAGS=-fno-omit-frame-pointer
make -j4
```

### gdb
A new version of gdb is highly recommended since debugging a JIT tends to stress GDB:

```
cd ~/pyston_deps
wget http://ftp.gnu.org/gnu/gdb/gdb-7.6.2.tar.gz
tar xvf gdb-7.6.2.tar.gz
cd gdb-7.6.2
./configure
make -j4
```

Then add this to your Makefile.local:
```
GDB := $(HOME)/pyston_deps/gdb-7.6.2/gdb/gdb
```

### gperftools (-lprofiler)
```
download from http://code.google.com/p/gperftools/downloads/list
standard ./configure, make, make install
```

### gold

gold is highly recommended as a faster linker.  Pyston contains build-system support for automatically using gold if available.  gold may already be installed on your system; you can check by typing `which gold`.  If it's not installed already:

```
cd ~/pyston_deps
wget http://ftp.gnu.org/gnu/binutils/binutils-2.24.tar.gz
tar xvf binutils-2.24.tar.gz
mkdir binutils-2.24-build
cd binutils-2.24-build
../binutils-2.24/configure --enable-gold --enable-plugins --disable-werror
make all-gold -j4
```

If that last step fails due to complaints about YYSTYPE, try upgrading or installing bison (`sudo apt-get install bison`), removing the binutils-2.24-build directory, and configure + make again.

### perf
The `perf` tool is the best way we've found to profile JIT'd code; you can find more details in docs/PROFILING.

```
sudo apt-get install linux-tools
sudo apt-get install linux-tools-`uname -r`
# may need to strip off the -generic from that last one
```

