# Pyston

Pyston is a new, under-development Python implementation built using LLVM and modern JIT techniques with the goal of achieving good performance.

### Current state

Pyston "works", though doesn't support very much of the Python language, and currently is not very useful for end-users.

Currently, Pyston targets Python 2.7, only runs on x86_64 platforms, and only has been tested on Ubuntu.  Support for more platforms -- along with Python 3 compatibility -- is planned for the future, but this is the initial target due to the fact that Dropbox is on this setup internally.

> Note: Pyston does not currently work on Mac OSX, but is being actively worked on; stay tuned on [pyston-dev](http://lists.pyston.org/cgi-bin/mailman/listinfo/pyston-dev) to hear when it does.

Benchmarks are not currently that meaningful since the supported set of benchmarks is too small to be representative; with that caveat, Pyston seems to have better performance than CPython but lags behind PyPy.

##### Contributing

Pyston welcomes any kind of contribution; please see [CONTRIBUTING.md](https://github.com/dropbox/pyston/blob/master/CONTRIBUTING.md) for details.

### Roadmap

Pyston is still an early-stage project so it is hard to project with much certainty, but here's what we're planning at the moment:

##### Current focus: more language features
- Exceptions
- Class inheritance, metaclasses
- Default arguments, keywords, \*args, **kwargs
- Closures
- Generators
- Integer promotion

##### After that
- More optimization work
 - Custom LLVM code generator that can very quickly produce bad machine code?
 - Making class-level slots for double-underscore functions (like \_\_str__) so runtime code can be as fast as Python code.
 - Change deopt strategy?
- Extension module support

##### Some time later:
- Threading (hopefully without a GIL)
- Adding support for Python 3, for non-x86_64 platforms

### Contributing

To contribute to Pyston, you need to to sign the [Dropbox Contributor License Agreement](https://opensource.dropbox.com/cla/), if you haven't already.

### Getting started

To get a full development environment for Pyston, you need pretty recent versions of various tools, since self-modifying code tends to be less well supported.  The docs/INSTALLING.md file contains information about what the tools are, how to get them, and how to install them; currently it can take up to an hour to get them all built on a quad-core machine.

To simply build and run Pyston, a smaller set of dependencies is required; see docs/INSTALLING.md, but skip the "OPTIONAL DEPENDENCIES" section. Once all the dependencies are installed, you should be able to do
```
$ make check -j4
```

And see that hopefully all of the tests will pass.

### Running Pyston

Pyston builds in a few different configurations; right now there is `pyston_dbg`, which is the debug configuration and contains assertions and debug symbols, and `pyston`, the release configuration which has no assertions or debug symbols, and has full optimizations.  You can build them by saying `make pyston_dbg` or `make pyston`, respectively.  If you are interested in seeing how fast Pyston can go, you should try the release configuration, but there is a good chance that it will crash, in which case you can run the debug configuration to see what is happening.

> There are a number of other configurations useful for development: "pyston_debug" contains full LLVM debug information, but can be over 100MB.  "pyston_prof" contains gprof-style profiling instrumentation; gprof can't profile JIT'd code, reducing it's usefulness in this case, but the configuration has stuck around since it gets compiled with gcc, and can expose issues with the normal clang-based build.

You can get a simple REPL by simply typing `./pyston`; it is not very robust right now, and only supports single-line statements, but can give you an interactive view into how Pyston works.  To get more functionality, you can do `./pyston -i [your_source_file.py]`, which will go into the REPL after executing the given file, letting you access all the variables you had defined.

To run the tests, run `make test`.

#### Command-line options:
<dl>
<dt>-n</dt>
  <dd>Disable the Pyston interpreter.  The interpreter doesn't support certain features, such as inline caches, so disabling it can expose additional bugs.</dd>
  
<dt>-O</dt>
  <dd>Force Pyston to always run at the highest compilation tier.  This doesn't always produce the fastest running time due to the lack of type recording from lower compilation tiers, but can help stress-test the code generator.</dd>
  
<dt>-q</dt>
  <dd>Set verbosity to 0</dd>
<dt>-v</dt>
  <dd>Increase verbosity by 1</dd>
  
  <dd>Pyston by default runs at verbosity 1, which contains a good amount of debugging information.  Verbosity 0 contains no debugging information (such as the LLVM IR generated) and should produce the same results as other runtimes.</dd>
  
<dt>-d</dt>
  <dd>In addition to showing the generated LLVM IR, show the generated assembly code.</dd>
  
<dt>-i</dt>
  <dd>Go into the repl after executing the given script.</dd>
  
<dt>-b</dt>
  <dd>Benchmark mode: do whatever it would have been done, but do it 1000 times.</dd>
  
<dt>-p</dt>
  <dd>Emit profiling information: at exit, Pyston will emit a dump of the code it generated for consumption by other tools.</dd>
  
<dt>-r</dt>
  <dd>Use a stripped stdlib.  When running pyston_dbg, the default is to use a stdlib with full debugging symbols enabled.  Passing -r changes this behavior to load a slimmer, stripped stdlib.</dd>

### Version History

##### v0.1: 4/2/2014

Initial Release.
- Working system; proof of concept of major JIT techniques.
- Fairly promising initial performance, though not fully validated.
- Missing large parts of the language
  - Exceptions (planned for 0.2)
  - Class inheritance (planned for 0.2)
  - Default arguments, keywords, starargs, kwargs (planned for 0.2)
  - Generators (planned for 0.2)
  - Integer promotion (planned for 0.2)
  - Threads

---
## Technical features

### Compilation tiers

Pyston currently features four compilation tiers.  In increasing order of speed, but also compilation time:

1. An LLVM-IR interpreter.  LLVM IR is not designed for interpretation, and isn't very well suited for the task -- it is too low level, and the interpreter spends too much time dispatching for each instruction.  The interpreter is currently used for the first three times that a function is called, or the first ten iterations of a loop, before switching to the next level.
2. Baseline LLVM compilation.  Runs no LLVM optimizations, and no type speculation, and simply hands off the generated code to the LLVM code generator.  This tier does type recording for the final tier.
3. Improved LLVM compilation.  Behaves very similarly to baseline LLVM compilation, so this tier will probably be removed in the near future.
4. Full LLVM optimization + compilation.  This tier runs full LLVM optimizations, and uses type feedback from lower tiers.  This tier kicks in after 10000 loop iterations, or 10000 calls to a function. (exact numbers subject to change).

There are two main ways that Pyston can move up to higher tiers:
- If a function gets called often, it will get recompiled at a higher tier and the new version will be called instead.
- If a loop gets iterated enough times, Pyston will OSR to a higher tier within the same function.

Currently Pyston only moves to higher tiers, and doesn't move back down to lower tiers.  This will be important to add, in order to support doing additional type recording if types change.

The current plan is to replace the interpreter with a quick code generator that doesn't use LLVM's machinery; in theory it should be possible to build a simple code generator that just uses the LLVM IR as an input.

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

The pseudo-C shown above doesn't look that different; the benefit of this approach is that the square() function can be compiled at a low compilation tier, but the square_osrentry can be compiled at a higher one since the compilation time is much more likely to pay off.

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

### Aspiration: Extension modules

CPython-style C extension modules can be difficult in a system that doesn't use refcounting, since a GC-managed runtime is forced to provide a refcounted API.  PyPy handles this by using a compatibility layer to create refcounted objects; our hope is to do the reverse, and instead of making the runtime refcount-aware, to make the extension module GC-aware.

To do this without requiring modifications to extension modules, the plan would be to apply the existing conservative GC to the extension module's stack and memory.  In theory this should be possible to do, but requires some technical trickiness to track allocations done by the extension module.  It may not even be more performant -- if the extension module uses large binary buffers, the GC will be forced to scan the buffer for pointers, whereas obeying the refcount contract would add only a size-independent overhead.  If this turns out to be the case, it might be possible to do some static analysis of the extension modules, and see where GC-managed pointers can escape to.

As the section heading says, though, this is all just a thought right now.  Whether or not this works, and is performant, still remains to be seen.

### Aspiration: Thread-level Parallelism

Many runtimes for dynamic languages -- including CPython and PyPy -- use a Global Interpreter Lock (GIL) to protect internal structures against concurrent modification.  This works, but has the drawback of only allowing one thread at a time to run.

The number of cores you can obtain in a single machine keeps growing, which means the performance deficit of single-threaded programs is falling vs multi-threaded ones.  There has been some work to support multi-process parallelism in Python, though many people prefer multi-threaded paralellism for its (relative) ease of use.

We have no concrete ideas or plans for how to implement this, so this section is all optimistic, but our hope is that it will be possible to implement true parallelism.

One of the biggest challenges for this is not just protecting the internal runtime structures, but also providing the higher-level guarantees that Python programmers have become accustomed to.  One example is that all builtin datastructures must be thread-safe, since they currently are.  A slightly more sinister one is that Python has a very straightforward memory model, where no operations can be viewed in different orders on different threads, because all thread switching involves a lock release-then-acquire which serializes the memory accesses; performantly maintaining this memory model is likely to be a challenge.
