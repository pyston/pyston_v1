# Pyston [![Build Status](https://travis-ci.org/dropbox/pyston.svg?branch=master)](https://travis-ci.org/dropbox/pyston/builds) [![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/dropbox/pyston?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Pyston is a performance-oriented Python implementation built using LLVM and modern JIT techniques.  For a high-level description of how it works, please check out our [technical overview](https://github.com/dropbox/pyston/wiki/Technical-overview) or our [FAQ](https://github.com/dropbox/pyston/wiki/FAQ) for some more details.

We have a small website [pyston.org](http://pyston.org/), which for now just hosts the blog.  We have two mailing lists: [pyston-dev@](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-dev) for development-related discussions, and [pyston-announce@](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-announce) which is for wider announcements (new releases, major project changes).  We also have a [gitter chat room](https://gitter.im/dropbox/pyston) where most discussion takes place.

### Current state

Pyston should be considered in alpha: it "works" in that it can successfully run Python code, but it is still quite far from being useful for end-users.

Currently, Pyston targets Python 2.7, only runs on x86_64 platforms, and only has been tested on Ubuntu.  Support for more platforms -- along with Python 3 compatibility -- is desired but deferred until we feel successful on our initial platform.  Pyston does not currently work on Mac OSX, and it is not clear when it will.

### Getting Pyston

We have some experimental docker images available; try running

```
docker run -it pyston/pyston
```

to be dropped in to a container that runs a pre-activated pyston virtualenv.  This means that the "pip" and "python" commands correspond to Pyston, so you can directly start doing `pip install mypackage` to get started.  As mentioned though, the docker images are experimental, so please give us feedback [on gitter](https.://gitter.im/dropbox/pyston).

You can also download the latest release from [GitHub](https://github.com/dropbox/pyston/releases).  It should for the most part just extract and run.  One thing to note is that current virtualenv versions do not support Pyston, so you will need to use our version that we ship in the releases.  See our [Dockerfile](https://github.com/dropbox/pyston/blob/master/docker/Dockerfile) for how to do this.

You can also build Pyston directly from source.  We have some build instructions at [`INSTALLING.md`](https://github.com/dropbox/pyston/blob/master/docs/INSTALLING.md).  If you have any issues, please feel free to file an issue in the issue tracker, or mention it via email or gitter.

##### NumPy

The easiest way to get started with NumPy-on-Pyston is to use another one of docker containers:

```
docker run -it pyston/pyston-numpy
```

To install it yourself, one needs a modified Cython, as well as to install numpy from source in order to invoke our modified Cython.

```
pip install https://github.com/dropbox/pyston/releases/download/v0.6.1/Cython-0.24-pyston.tar.gz
pip install git+git://github.com/numpy/numpy@v1.11.0
```

### Contributing

Pyston welcomes any kind of contribution; please see [`CONTRIBUTING.md`](https://github.com/dropbox/pyston/blob/master/CONTRIBUTING.md) for details.
> tl;dr: You will need to sign the [Dropbox CLA](https://opensource.dropbox.com/cla/) and run the tests.

We have some documentation for those interested in contributing: see our [Development Guide](https://github.com/dropbox/pyston/wiki/Development-Guide) and [development tips](docs/TIPS.md).

All pull requests are built and tested by travis-ci.org running Ubuntu 12.04.
See [travis-ci.org/dropbox/pyston/builds](https://travis-ci.org/dropbox/pyston/builds).

### Roadmap

##### v0.6.1: [released 1/31/2017](https://blog.pyston.org/2017/01/31/pyston-0-6-1-released-and-future-plans/)
- smaller performance optimizations and bug fixes
- updated CPython runtime to 2.7.8

##### v0.6: [released 11/21/2016](https://blog.pyston.org/2016/11/11/pyston-0-6-released/)
- focused on reducing the memory usage with the result that the peak memory usage on various benchmarks nearly halved.
- new bytecode
- lots of fixes for compatibility issues and other bugs

##### v0.5.1: [released 7/14/2016](https://blog.pyston.org/2016/07/14/pyston-0-5-1-released/)
- misc baseline JIT and inline cache improvements
- SciPy support
- analysis passes now use vregs instead of names

##### v0.5: [released 5/25/2016](https://blog.pyston.org/2016/05/25/pyston-0-5-released/)
- Reference counting
- Signals support, generator cleanup, more C API support

##### v0.4: [released 11/3/2015](http://blog.pyston.org/2015/11/03/102/)
- Many new features and better language support
 - Passes most of the tests of several famous python libraries
 - Unicode support
 - GC finalizer
 - much improved C API support
- Better performance
 - Custom C++ exception handler
 - Object Cache (improves startup time)
 - Baseline JIT

##### v0.3: [released 2/24/2015](http://blog.pyston.org/2015/02/24/pyston-0-3-self-hosting-sufficiency/)
- Better language support
 - Can self-host all of our internal Python scripts
- Better performance
 - Match CPython's performance on our small benchmark suite

##### v0.2: [released 9/11/2014](http://blog.pyston.org/2014/09/11/9/)
- Focus was on improving language compatibility to the point that we can start running "real code" in the form of existing benchmarks.
- Many new features:
 - Exceptions
 - Class inheritance, metaclasses
 - Basic native C API support
 - Closures, generators, lambdas, generator expressions
 - Default arguments, keywords, \*args, \*\*kwargs
 - Longs, and integer promotion
 - Multithreading support
- We have allowed performance to regress, sometimes considerably, but (hopefully) in places that allow for more efficient implementations as we have time.

##### v0.1: [released 4/2/2014](https://tech.dropbox.com/2014/04/introducing-pyston-an-upcoming-jit-based-python-implementation/)
- Focus was on building and validating the core Python-to-LLVM JIT infrastructure.
- Many core parts of the language were missing.

### Running Pyston

Pyston builds in a few different configurations; right now there is `pyston_dbg`, which is the debug configuration and contains assertions and debug symbols, and `pyston_release`, the release configuration which has no assertions or debug symbols, and has full optimizations.  You can build them by saying `make pyston_dbg` or `make pyston_release`, respectively.  If you are interested in seeing how fast Pyston can go, you should try the release configuration, but there is a good chance that it will crash, in which case you can run the debug configuration to see what is happening.

> There are a number of other configurations useful for development: "pyston_debug" contains full LLVM debug information, but will weigh in at a few hundred MB.  "pyston_prof" contains gprof-style profiling instrumentation; gprof can't profile JIT'd code, reducing it's usefulness in this case, but the configuration has stuck around since it gets compiled with gcc, and can expose issues with the normal clang-based build.

You can get a simple REPL by simply typing `make run`; it is not very robust right now, and only supports single-line statements, but can give you an interactive view into how Pyston works.  To get more functionality, you can do `./pyston_dbg -i [your_source_file.py]`, which will go into the REPL after executing the given file, letting you access all the variables you had defined.

#### Makefile targets

- `make pyston_release`: to compile in release mode and generate the `pyston_release` executable
- `make check`: run the tests
- `make run`: run the REPL
- `make format`: run clang-format over the codebase

For more, see [development tips](docs/TIPS.md)

#### Pyston command-line options:

Pyston-specific flags:
<dl>
<dt>-q</dt>
  <dd>Set verbosity to 0</dd>
<dt>-v</dt>
  <dd>Increase verbosity by 1</dd>

<dt>-T</dt>
  <dd>Print out the internal stats at exit.</dd>

<dt>-n</dt>
  <dd>Disable the Pyston interpreter.  This is mostly used for debugging, to force the use of higher compilation tiers in situations they wouldn't typically be used.</dd>

<dt>-O</dt>
  <dd>Force Pyston to always run at the highest compilation tier.  This doesn't always produce the fastest running time due to the lack of type recording from lower compilation tiers, but similar to -n can help test the code generator.</dd>

<dt>-I</dt>
  <dd>Force always using the Pyston interpreter.  This is mostly used for debugging / testing. (Takes precedence over -n and -O)</dd>

<dt>-r</dt>
  <dd>Use a stripped stdlib.  When running pyston_dbg, the default is to use a stdlib with full debugging symbols enabled.  Passing -r changes this behavior to load a slimmer, stripped stdlib.</dd>

Standard Python flags:
<dt>-i</dt>
  <dd>Go into the repl after executing the given script.</dd>
</dl>

There are also some lesser-used flags; see [`src/jit.cpp`](https://github.com/dropbox/pyston/blob/master/src/jit.cpp) for more details.
