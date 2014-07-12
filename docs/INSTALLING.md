Pyston currently only supports installing from source; the following instructions have only been tested on Ubuntu, and currently has some non-trivial build issues on Mac (hopefully will be addressed soon once we get access to a Mac VM).

The build instructions assume that you will put the Pyston source code in `~/pyston` and put the dependencies in `~/pyston_deps`.  Barring any bugs, you should be free to put them anywhere you'd like, though the instructions in this file would have to be altered before following.  Also, if you want to change the dependency dir, you'll have to change the value of the the `DEPS_DIR` variable in `src/Makefile`.


### Prerequisites
GNU make is required to build pyston.


Start off by making the relevant directories:

```
mkdir ~/pyston_deps
git clone https://github.com/dropbox/pyston.git ~/pyston
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

LLVM and clang depend on a pretty modern compiler; the steps below assume you installed GCC 4.8.2 as described above.  It should be possible to build using clang >= 3.1, such as what you might find on a Mac, but that will require changes to the way LLVM is configured (specified in src/Makefile) that I haven't tested.

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
sudo apt-get install texlive-extra-utils autoconf
git clone git://git.sv.gnu.org/libunwind.git libunwind-trunk
mkdir libunwind-trunk-install
cd libunwind-trunk
git checkout 65ac867416
autoreconf -i
# disable shared libraries because we'll be installing this in a place that the loader can't find it:
./configure --prefix=$HOME/pyston_deps/libunwind-trunk-install --enable-shared=0
make -j4
make install
```

Note: if you followed the previous version of the directions and installed libunwind globally, you'll need to uninstall it by doing the following:

```
cd ~/pyston_deps/libunwind-1.1
./configure
sudo make uninstall
```

and then repeat the correct process

### zsh
`zsh` is needed when running pyston tests.
```
apt-get install zsh
```

### readline
`readline` is used for the repl.

```
sudo apt-get install readline
```

# Optional dependencies

There are a number of optional dependencies that the build system knows about, but aren't strictly necessary for building and running Pyston.  Most of them are related to developing and debugging:

### valgrind

Since Pyston uses a custom memory allocator, it makes use of the valgrind client hooks to help valgrind understand it.  This means that you'll need a copy of the valgrind includes around; I also suggest that if you want to use valgrind, you use a recent version since there are some instructions that LLVM emits that somewhat-recent versions of valgrind don't understand.

To install:

```
cd ~/pyston_deps
sudo apt-get install liblzma-dev
wget http://valgrind.org/downloads/valgrind-3.9.0.tar.bz2
tar xvf valgrind-3.9.0.tar.bz2
mkdir valgrind-3.9.0-install
cd valgrind-3.9.0
./configure --prefix=$HOME/pyston_deps/valgrind-3.9.0-install
make -j4
make install
sudo apt-get install libc6-dbg
cd ~/pyston/src
echo "ENABLE_VALGRIND := 1" >> Makefile.local
```

### Debug build of libunwind

Assuming you've already built the normal version above:

```
cd ~/pyston_deps
cp -rv libunwind-trunk libunwind-trunk-debug
mkdir libunwind-trunk-debug-install
cd libunwind-trunk-debug
CFLAGS="-g -O0" CXXFLAGS="-g -O0" ./configure --prefix=$HOME/pyston_deps/libunwind-trunk-debug-install --enable-shared=0 --enable-debug --enable-debug-frame
make -j4
make install
echo "USE_DEBUG_LIBUNWIND := 1" >> ~/pyston/src/Makefile.local
```

This will link pyston_dbg and pyston_debug against the debug version of libunwind (the release pyston build will still link against the release libunwind); to enable debug output, set the UNW_DEBUG_LEVEL environment variable, ex to 13.

### distcc
```
sudo apt-get install distcc distcc-pump
```

You can then use distcc by doing `make USE_DISTCC=1`

### gtest

For running the unittests:

```
cd ~/pyston_deps
wget https://googletest.googlecode.com/files/gtest-1.7.0.zip
unzip gtest-1.7.0.zip
cd gtest-1.7.0
./configure CXXFLAGS=-fno-omit-frame-pointer
make -j4
```

### gdb
A new version of gdb is highly recommended since debugging a JIT tends to use new features of GDB:

```
cd ~/pyston_deps
wget http://ftp.gnu.org/gnu/gdb/gdb-7.6.2.tar.gz
tar xvf gdb-7.6.2.tar.gz
cd gdb-7.6.2
./configure
make -j4
cd ~/pyston/src
echo "GDB := \$(DEPS_DIR)/gdb-7.6.2/gdb/gdb --data-directory \$(DEPS_DIR)/gdb-7.6.2/gdb/data-directory" >> Makefile.local
```

<!---
TODO: GDB should be able to determine its data directory.  maybe it should be installed rather that run
from inside the source dir?
--->

### gperftools (-lprofiler)
```
download from http://code.google.com/p/gperftools/downloads/list
standard ./configure, make, make install
```

### gold

gold is highly recommended as a faster linker, and Pyston contains build-system support for automatically using gold if available.  gold may already be installed on your system; you can check by typing `which gold`.  If it's not installed already:

```
cd ~/pyston_deps
sudo apt-get install bison
wget http://ftp.gnu.org/gnu/binutils/binutils-2.24.tar.gz
tar xvf binutils-2.24.tar.gz
mkdir binutils-2.24-build
cd binutils-2.24-build
../binutils-2.24/configure --enable-gold --enable-plugins --disable-werror
make all-gold -j4
```

### perf
The `perf` tool is the best way we've found to profile JIT'd code; you can find more details in docs/PROFILING.

```
sudo apt-get install linux-tools
sudo apt-get install linux-tools-`uname -r`
# may need to strip off the -generic from that last one
```

### ninja-based LLVM build

Ninja is supposed to be faster than make; I've only tried it very briefly, and it does seem to be faster when modifying LLVM files.  May or may not be worth using; thought I'd jot down my notes though:

You may or may not need a more-recent version of ninja than your package manager provides:
```
cd ~/pyston_deps
git clone https://github.com/martine/ninja.git
cd ninja
git checkout v1.4.0
./bootstrap.py
```

```
cd ~/pyston_deps
wget http://www.cmake.org/files/v3.0/cmake-3.0.0.tar.gz
cd cmake-3.0.0
./configure
make -j4
```

```
cd ~/pyston_deps
mkdir llvm-trunk-cmake
cd llvm-trunk-cmake
CXX=g++ CC=gcc PATH=~/pyston_deps/gcc-4.8.2-install/bin:$PATH:~/pyston_deps/ninja CMAKE_MAKE_PROGRAM=~/pyston_deps/ninja/ninja ~/pyston_deps/cmake-3.0.0/bin/cmake ../llvm-trunk -G Ninja -DLLVM_TARGETS_TO_BUILD=host -DCMAKE_BUILD_TYPE=RELEASE -DLLVM_ENABLE_ASSERTIONS=ON
~/pyston_deps/ninja/ninja # runs in parallel
```
