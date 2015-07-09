# Pyston [![Build Status](https://travis-ci.org/dropbox/pyston.svg?branch=master)](https://travis-ci.org/dropbox/pyston/builds) [![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/dropbox/pyston?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Pyston is a new, under-development Python implementation built using LLVM and modern JIT techniques with the goal of achieving good performance.

We have a small website [pyston.org](http://pyston.org/), which for now just hosts the mailing lists and the [blog](http://blog.pyston.org/).  We have two mailing lists: [pyston-dev@](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-dev) for development-related discussions, and [pyston-announce@](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-announce) which is for wider announcements (new releases, major project changes).  We also have a [gitter chat room](https://gitter.im/dropbox/pyston) where most discussion takes place.

### Current state

Pyston should be considered in alpha: it "works" in that it can successfully run Python code, but it is still quite far from being useful for end-users.

Currently, Pyston targets Python 2.7, only runs on x86_64 platforms, and only has been tested on Ubuntu.  Support for more platforms -- along with Python 3 compatibility -- is desired but currently not on the roadmap.

> Note: Pyston does not currently work on Mac OSX, and it is not clear when it will.

##### Contributing

Pyston welcomes any kind of contribution; please see [CONTRIBUTING.md](https://github.com/dropbox/pyston/blob/master/CONTRIBUTING.md) for details.
> tl;dr: You will need to sign the [Dropbox CLA](https://opensource.dropbox.com/cla/) and run the tests.

Pyston a fairly low-level program with a lot of necessary hacks for compatibility or performance purposes. We recommend taking a look at [development tips](docs/TIPS.md).

### Roadmap

##### v0.1: [released 4/2/2014](https://tech.dropbox.com/2014/04/introducing-pyston-an-upcoming-jit-based-python-implementation/)
- Focus was on building and validating the core Python-to-LLVM JIT infrastructure.
- Many core parts of the language were missing.

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

##### v0.3: [released 2/24/2015](http://blog.pyston.org/2015/02/24/pyston-0-3-self-hosting-sufficiency/)
- Better language support
 - Can self-host all of our internal Python scripts
- Better performance
 - Match CPython's performance on our small benchmark suite

##### v0.4: Coming soon

### Getting started

We have some build instructions at [INSTALLING.md](https://github.com/dropbox/pyston/blob/master/docs/INSTALLING.md).  If you have any issues, please feel free to file an issue in the issue tracker, or mention it via email or gitter.

Once you've followed those instructions, you should be able to do
```
$ make check
```

And see that hopefully all of the tests pass.  (If they don't, please let us know.)

All pull requests are built and tested by travis-ci.org running Ubuntu 12.04.
See [travis-ci.org/dropbox/pyston/builds](https://travis-ci.org/dropbox/pyston/builds).

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

<dt>-s</dt>
  <dd>Print out the internal stats at exit.</dd>

<dt>-n</dt>
  <dd>Disable the Pyston interpreter.  This is mostly used for debugging, to force the use of higher compilation tiers in situations they wouldn't typically be used.</dd>

<dt>-O</dt>
  <dd>Force Pyston to always run at the highest compilation tier.  This doesn't always produce the fastest running time due to the lack of type recording from lower compilation tiers, but similar to -n can help test the code generator.</dd>

<dt>-I</dt>
  <dd>Force always using the Pyston interpreter.  This is mostly used for debugging / testing. (Takes precedence over -n and -O)</dd>

<dt>-r</dt>
  <dd>Use a stripped stdlib.  When running pyston_dbg, the default is to use a stdlib with full debugging symbols enabled.  Passing -r changes this behavior to load a slimmer, stripped stdlib.</dd>

<dt>-x</dt>
  <dd>Disable the pypa parser.</dd>

Standard Python flags:
<dt>-i</dt>
  <dd>Go into the repl after executing the given script.</dd>
</dl>

There are also some lesser-used flags; see src/jit.cpp for more details.

---
## Technical features

### Compilation tiers

Pyston currently features four compilation tiers.  In increasing order of speed, but also compilation time:

0. An AST interpreter.  We do some basic transformations on the AST beforehand, to make it easier and faster to interpret.
1. Baseline LLVM compilation.  Runs no LLVM optimizations, and no type speculation, and simply hands off the generated code to the LLVM code generator.  This tier does type recording for the final tier.  We are thinking of replacing this with a simple non-LLVM JIT tier at some point.
2. Improved LLVM compilation.  Behaves very similarly to baseline LLVM compilation, so this tier will probably be removed in the near future.
3. Full LLVM optimization + compilation.  This tier runs full LLVM optimizations, and uses type feedback from lower tiers.  This tier currently kicks in after 10000 loop iterations, or 10000 calls to a function.

There are two main ways that Pyston can move up to higher tiers:
- If a function gets called often, it will get recompiled at a higher tier and the new version will be called instead.
- If a loop gets iterated enough times, Pyston will OSR to a higher tier within the same function.

Pyston can move back down to the AST interpreter by using our frame introspection machinery to do a deoptimization.

#### Frame introspection

Pyston uses LLVM's [patchpoint](http://llvm.org/docs/StackMaps.html) functionality to convey information from the LLVM code generator to the runtime.  By attaching all local variables as stackmap arguments, at any callsite we can access all of the frame's local variables.  We use this to implement user-level features such as eval() and locals(), and also to implement internal features such as deoptimization.

We have a [blog post](http://blog.pyston.org/2014/11/06/frame-introspection-in-pyston/) that goes into more detail.

#### OSR

Pyston uses OSR (which stands for On-Stack Replacement, though Pyston does not use that particular mechanism) to move up to a higher tier while inside a function -- this can be important for functions that are expensive the very first time they are called.

OSR is implemented in Pyston by keeping a count, per backedge, of the number of times that the backedge is taken.  Once a certain threshold is reached (currently 10 for the interpreter, 10000 otherwise), Pyston will compile a special OSR-entry version of the function.  This function takes as arguments all the local variables for that point in the program, and continues execution where the previous function left off.

For example, this Python function:
```python
def square(n):
    r = 0
    for i in xrange(n):
        r += n
    return r
```
will get translated to something similar to:
```C
static _backedge_trip_count = 0;
int square(int n) {
    int r = 0;
    for (int i = 0; i < n; i++) {
        r += n;

        // OSR exit here:
        _backedge_trip_count++;
        if (_backedge_trip_count >= 10000) {
            auto osr_entry = compileOsrEntry();
            return osr_entry(n, i, r);
        }
    }
    return r;
}
```

The compiled OSR entry will look something similar to:
```C
int square_osrentry(int n, int i, int r) {
    for (; i < n; i++) {
        r += n;
    }
    return r;
}
```

The pseudo-C shown above doesn't look that different; the benefit of this approach is that the square() function can be compiled at a low (cheap) compilation tier or even interpreted, but the square_osrentry can be compiled at a higher one since the compilation time is much more likely to pay off.

This approach seems to work, but has a couple drawbacks:
- It's currently tracked per backedge rather than per backedge-target, which can lead to more OSR compilations than necessary.
- The OSR'd version can be slower due to the optimizations having less context about the source of the arguments, ie that they're local variables that haven't escaped.

### Inlining

Pyston can inline functions from its runtime into the code that it's JIT'ing.  This only happens if, at JIT time, it can guarantee the runtime function that would end up getting called, which typically happens if it is an attribute of a guaranteed type.  For instance, `[].append()` will end up resolving to the internal listAppend(), since we know what the type of `[]` is.

Once the Python-level call is resolved to a C-level call to a runtime function, normal inlining heuristics kick in to determine if it is profitable to inline the function.  As a side note, the inlining is only possible because the LLVM IR for the runtime is not only compiled to machine code to be run, but also directly embedded as LLVM IR into the pyston binary, so that the LLVM IR can be inlined.

### Object representation

Current Pyston uses an 'everything is boxed' model.  It has some ability to deal with unboxed variants of ints, floats, and bools, but those unboxed types are not mixable with boxed types.  ie if you put an integer into a list, the integer will always get boxed first.

### Inline caches

### Hidden classes

### Type feedback

Currently, tiers 2 and 3 support *type recording*, and make a record of the types seen at specifically-designated parts of the program.

Tier 4 then looks at the type record; the current heuristic is that if the same type has been seen 100 times in a row, the compiler will speculate

### Garbage collection

Pyston currently utilizes a *conservative* garbage collector -- this means that GC roots aren't tracked directly, but rather all GC-managed memory is scanned for values that could point into the GC heap, and treat those conservatively as pointers that keep the pointed-to GC memory alive.

Currently, the Pyston's GC is a non-copying, non-generational, stop-the-world GC.  ie it is a simple implementation that will need to be improved in the future.

### Native extension module support

CPython-style C extension modules can be difficult in a system that doesn't use refcounting, since a GC-managed runtime is forced to provide a refcounted API.  PyPy handles this by using a compatibility layer to create refcounted objects; our hope is to do the reverse, and instead of making the runtime refcount-aware, to make the extension module GC-aware.

We have a basic implementation of this that is able to run a number of the CPython standard modules (from the Modules/ directory), and so far seems to be working.  The C API is quite large and will take some time to cover.

### Parallelism support

Pyston currently uses a GIL to protect threaded code.  The codebase still contains an experimental "GRWL" configuration, which replaces the GIL with a read-write lock.  This allows Python code to execute in parallel but still allow for critical sections (recompilation, C API calls, etc), and seems to work ok.  It doesn't provide the same memory-ordering guarantees that CPython provides.

This approach has mostly been abandoned as infeasible, but you can test it by doing `make pyston_grwl`.
