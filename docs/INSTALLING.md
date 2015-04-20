Pyston currently only supports installing from source; the following instructions have fairly tested as working on Ubuntu, and are extensively verified as not working on Mac.  (Please see issue #165 for discussion on enabling OSX support, which is pretty difficult.)

The build instructions assume that you will put the Pyston source code in `~/pyston` and put the dependencies in `~/pyston_deps`.  Barring any bugs, you should be free to put them anywhere you'd like, though the instructions in this file would have to be altered before following.  Also, if you want to change the dependency dir, you'll have to change the value of the the `DEPS_DIR` variable in `Makefile`.


### Prerequisites
GNU make is required to build pyston.


Start off by making the relevant directories:

```
mkdir ~/pyston_deps
git clone --recursive https://github.com/dropbox/pyston.git ~/pyston
```

### Compiler for clang

clang requires a fairly modern [host compiler](http://llvm.org/docs/GettingStarted.html#host-c-toolchain-both-compiler-and-standard-library), so typically you will have to install a new one.  The easiest thing to do is to just create a fresh build of GCC:

```
sudo apt-get install libgmp-dev libmpfr-dev libmpc-dev make build-essential libtool zip gcc-multilib autogen
cd ~/pyston_deps
wget http://ftpmirror.gnu.org/gcc/gcc-4.8.2/gcc-4.8.2.tar.bz2
tar xvf gcc-4.8.2.tar.bz2
mkdir gcc-4.8.2-{build,install}
cd gcc-4.8.2-build
# Space- and time-saving configuration:
../gcc-4.8.2/configure --disable-bootstrap --enable-languages=c,c++ --prefix=$HOME/pyston_deps/gcc-4.8.2-install
# full configuration:
# ../gcc-4.8.2/configure --prefix=$HOME/pyston_deps/gcc-4.8.2-install
# Specifying LIBRARY_PATH is a workaround to get gcc to compile on newer Ubuntus with multiarch
LIBRARY_PATH=/usr/lib32 make -j4
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
sudo apt-get install libncurses5-dev zlib1g-dev liblzma-dev
```

### LLVM + clang

LLVM and clang depend on a pretty modern compiler; the steps below assume you installed GCC 4.8.2 as described above.  It should be possible to build using clang >= 3.1, such as what you might find on a Mac, but that will require changes to the way LLVM is configured (specified in Makefile) that I haven't tested.

```
cd ~/pyston_deps
git clone http://llvm.org/git/llvm.git llvm-trunk
git clone http://llvm.org/git/clang.git llvm-trunk/tools/clang
cd ~/pyston
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
sudo apt-get install zsh
```

### readline
`readline` is used for the repl.

```
sudo apt-get install libreadline-dev
```

### gmp
`gmp` is a multiprecision library used for implementing Python longs.  It's also a dependency of gcc, so if you installed that you should already have it:

```
sudo apt-get install libgmp3-dev
```

### libpypa

```
cd ~/pyston_deps
git clone git://github.com/vinzenz/pypa
mkdir pypa-install
cd pypa
./autogen.sh
./configure --prefix=$HOME/pyston_deps/pypa-install CXX=$HOME/pyston_deps/gcc-4.8.2-install/bin/g++
make -j4
make install
```

### libssl, libcrypto
```
sudo apt-get install libssl-dev
```

### gtest

For running the unittests:

```
cd ~/pyston_deps
wget https://googletest.googlecode.com/files/gtest-1.7.0.zip
unzip gtest-1.7.0.zip
cd gtest-1.7.0
./configure CXXFLAGS="-fno-omit-frame-pointer -isystem $HOME/pyston_deps/gcc-4.8.2-install/include/c++/4.8.2"
make -j4
```

### LZ4
```
cd ~/pyston_deps
git clone git://github.com/Cyan4973/lz4.git
mkdir lz4-install
cd lz4/lib
DESTDIR="$HOME/pyston_deps/lz4-install" PREFIX="/" make install
```

---

At this point you should be able to run `make check` (in the `~/pyston` directory) and pass the tests.  See the main README for more information about available targets and options.

# Optional dependencies

There are a number of optional dependencies that the build system knows about, but aren't strictly necessary for building and running Pyston.  Most of them are related to developing and debugging:

### CPython headers

We have some extension module tests, in which we compile the extension module code for both Pyston and CPython.

```
sudo apt-get install python-dev
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
cd ~/pyston
echo "GDB := \$(DEPS_DIR)/gdb-7.6.2/gdb/gdb --data-directory \$(DEPS_DIR)/gdb-7.6.2/gdb/data-directory" >> Makefile.local
```

<!---
TODO: GDB should be able to determine its data directory.  maybe it should be installed rather that run
from inside the source dir?
--->

#### getting gdb to pretty-print STL types

GDB ships with python scripts that tell it how to pretty-print STL containers, but they only work against the stdlib in the corresponding version of GCC. Unfortunately, we compile against an old version of gcc's C++ stdlib. Here's how to get GDB to use the *old* pretty-print scripts for GCC 4.8.2. (Note: Have only tested this with gdb 7.8.1-ubuntu4 and gcc-4.8.2.)

If you've built GCC, the GDB scripts will be in `~/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python`. Unfortunately they're for Python 2, and GDB 7.8.1 uses Python 3. So we'll use `2to3` to update them:

```
cd ~/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python/libstdcxx/v6
2to3 -w printers.py
```

Don't worry, `2to3` makes a backup of the file it changes if you need to go back. Now, edit your `~/.gdbinit` file to contain the following:

```
python
import sys
sys.path.insert(0, '/YOUR/HOME/DIR/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python')
from libstdcxx.v6.printers import register_libstdcxx_printers
register_libstdcxx_printers(None)
print('--- Registered cxx printers for gcc 4.8.2! ---')
```

And there you go!

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

### distcc
```
sudo apt-get install distcc distcc-pump
```

You can then use distcc by doing `make USE_DISTCC=1`

# Misc tools

These are only useful in specific development situations:

### valgrind

Since Pyston uses a custom memory allocator, it makes use of the valgrind client hooks to help valgrind understand it.  This means that you'll need a copy of the valgrind includes around; I also suggest that if you want to use valgrind, you use a recent version since there are some instructions that LLVM emits that somewhat-recent versions of valgrind don't understand.

To install:

```
cd ~/pyston_deps
wget http://valgrind.org/downloads/valgrind-3.10.0.tar.bz2
tar xvf valgrind-3.10.0.tar.bz2
mkdir valgrind-3.10.0-install
cd valgrind-3.10.0
./configure --prefix=$HOME/pyston_deps/valgrind-3.10.0-install
make -j4
make install
sudo apt-get install libc6-dbg
cd ~/pyston
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
echo "USE_DEBUG_LIBUNWIND := 1" >> ~/pyston/Makefile.local
```

This will link pyston_dbg and pyston_debug against the debug version of libunwind (the release pyston build will still link against the release libunwind); to enable debug output, set the UNW_DEBUG_LEVEL environment variable, ex to 13.

### gperftools (-lprofiler)
```
download from http://code.google.com/p/gperftools/downloads/list
standard ./configure, make, make install
```

### Debug build of CPython

Having a debug-enabled CPython can be useful for debugging issues with our extension modules.  To get it set up:

```
sudo apt-get install python2.7-dbg
cd ~/pyston_deps
mkdir python-src
cd python-src
apt-get source python2.7-dbg
```

Then, run `make dbgpy_TESTNAME` and it will launch the test under gdb.  There's also a [wiki page](https://wiki.python.org/moin/DebuggingWithGdb) with some extra Python-specific GDB commands.

### doxygen
```
sudo apt-get install doxygen graphviz

# then run cmake (see below) and invoke the docs target
ninja docs

# now within the Pyston build directory open docs/html/index.html with a browser
```

Generate doxygen documentation for Pyston. Requires using the cmake build system.

# (Experimental) CMake build system

To use the toolchain from this document, do:

```
mkdir ~/pyston-build
cd ~/pyston-build
CC="ccache $HOME/pyston_deps/gcc-4.8.2-install/bin/gcc" CXX="ccache $HOME/pyston_deps/gcc-4.8.2-install/bin/g++" ~/pyston_deps/cmake-3.0.0/bin/cmake -GNinja ~/pyston -DCMAKE_MAKE_PROGRAM=$HOME/pyston_deps/ninja/ninja -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,$HOME/pyston_deps/gcc-4.8.2-install/lib64" -DGCC_INSTALL_PREFIX=~/pyston_deps/gcc-4.8.2-install
~/pyston_deps/ninja/ninja check-pyston
```

If your system provides a new enough GCC and cmake, you can just do:

```
mkdir ~/pyston-build && cd ~/pyston-build
cmake -GNinja ~/pyston
ninja check-pyston
```

**Ubuntu 12.04**
```
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo add-apt-repository --yes ppa:kubuntu-ppa/backports
sudo apt-get -qq update

sudo apt-get install -yq git cmake ninja-build ccache libncurses5-dev liblzma-dev libreadline-dev libgmp3-dev autoconf libtool python-dev texlive-extra-utils clang-3.4 libstdc++-4.8-dev libssl-dev libsqlite3-dev

git clone --recursive https://github.com/dropbox/pyston.git ~/pyston

git clone git://github.com/llvm-mirror/llvm.git ~/pyston_deps/llvm-trunk
git clone git://github.com/llvm-mirror/clang.git ~/pyston_deps/llvm-trunk/tools/clang

mkdir ~/pyston-build && cd ~/pyston-build
CC='clang' CXX='clang++' cmake -GNinja ~/pyston

git config --global user.email "you@example.com"
git config --global user.name "Your Name"
ninja llvm_up

ninja check-pyston # run the test suite
```

**Ubuntu 14.04**
```
sudo apt-get install -yq git cmake ninja-build ccache libncurses5-dev liblzma-dev libreadline-dev libgmp3-dev autoconf libtool python-dev texlive-extra-utils clang-3.5 libssl-dev libsqlite3-dev

git clone --recursive https://github.com/dropbox/pyston.git ~/pyston

git clone git://github.com/llvm-mirror/llvm.git ~/pyston_deps/llvm-trunk
git clone git://github.com/llvm-mirror/clang.git ~/pyston_deps/llvm-trunk/tools/clang

mkdir ~/pyston-build && cd ~/pyston-build
CC='clang' CXX='clang++' cmake -GNinja ~/pyston

git config --global user.email "you@example.com"
git config --global user.name "Your Name"
ninja llvm_up

ninja check-pyston # run the test suite
```

Other important options:
- `-DCMAKE_BUILD_TYPE=Debug` (defaults to Release)
- `-DENABLE_LLVM_DEBUG=1` for full LLVM debug
- `-DENABLE_CCACHE=0` to disable ccache

