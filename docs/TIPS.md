# Development tips

## General

### Exceptions and stack unwinding

Having a custom unwinder, Pyston has a few special constraints - see [exception safety](EXCEPTION-SAFETY.md) and [unwinding](UNWINDING.md).

### Make shortcuts

The main Make targets you will use will be
- `make`: to compile in debug mode and generate the `pyston_dbg` executable.
- `make pyston_release`: to compile in release mode and generate the `pyston_release` executable.
- `make run`: run the REPL.
- `make format`: run clang-format over the codebase.
- `make quick_check`: run only the cheap-to-run tests, good for quick sanity checks.

In addition
- We have a number of helpers of the form `make VERB_TESTNAME`, where `TESTNAME` can be any of the tests/benchmarks, and `VERB` can be one of:
 - `make check`: run the full test suite including integration tests.
 - `make check_release`: release mode version of `make_check`.
 - `make run_TESTNAME`: runs the file under pyston_dbg.
 - `make run_release_TESTNAME`: runs the file under pyston_release.
 - `make dbg_TESTNAME`: same as `run`, but runs pyston under gdb.
 - `make check_TESTNAME`: checks that the script has the same behavior under pyston_dbg as it does under CPython.  See tools/tester.py for information about test annotations.
 - `make perf_TESTNAME`: runs the script in pyston_release, and uses perf to record and display performance statistics.
 - A few lesser used ones; see the Makefile for details.
- `make watch_cmd`: meta-command which uses inotifywait to run `make cmd` every time a source file changes.
 - For example, `make watch_pyston_dbg` will rebuild pyston_dbg every time you save a source file.  This is handy enough to have the alias `make watch`.
 - `make watch_run_TESTNAME` will rebuild pyston_dbg and run TESTNAME every time you change a file.
 - `make wdbg_TESTNAME` is mostly an alias for `make watch_dbg_TESTNAME`, but will automatically quit GDB for you.  This is handy if Pyston is crashing and you want to get a C-level stacktrace.

There are a number of common flags you can pass to your make invocations:
- `V=1` or `VERBOSE=1`: display the full commands being executed.
- `ARGS=-v`: pass the given args (in this example, `-v`) to the executable.
- Note: these will usually end up before the script name, and so apply to the Pyston runtime as opposed to appearing in sys.argv.  For example, `make run_test ARGS=-v` will execute `./pyston_dbg -v test.py`.
- `BR=breakpoint`: when running under gdb, automatically set a breakpoint at the given location.
- `SELF_HOST=1`: run all of our Python scripts using pyston_dbg.

For a full list, please check out the [Makefile](https://github.com/dropbox/pyston/blob/master/Makefile).

### CPython code

Pyston was developed with C-extension support in mind. We are making a C API that C extensions should be able to use without any change. Some problematic parts of CPython (such as custom destructors) are "commented out" via macros or replaced, and we use ideas like conservative scanning to address the functionality changes.

As a fortunate consequence, we are often able to call directly into CPython's code or copy large segments from it. For example, we mostly use all of CPython's existing logic for unicode strings, which would be a terrible idea to reimplement. A large portion of CPython's code resides in our `from_cpython/` directory, which some patches here and there.

Where possible and if it can be done cleanly, it can be a good idea to reuse CPython's code rather than rewrite our own. For this reason, it is also useful to have a copy of [CPython's 2.7 branch checked out](https://github.com/python/cpython/tree/2.7) for reference. Certain features have a lot of intricate edge cases like list slicing - it is useful to look at CPython's code to match their behavior.


## Performance

Pyston's primary reason _raison d'être_ is to have better performance than CPython. That's why it is important to always keep performance in mind when developing Pyston's codebase.

### Counters & timers

We have a number of counters and timers that were manually written and inserted into the program. To display the results of a run, use the `-s` flag. The result of timer counters is displayed in microseconds.

There's two types of timers in the code. The first are `StatCounter`s. They are just counters where we log the time elapsed manually, measured using a `Timer`. The second are `STAT_TIMER`s, which pause the parent `STAT_TIMER` when they are nested.

A lot of counters and timers are enabled even in release mode, but others would add too much overhead to include in every run. You can enable them by modifying the macros (e.g. `EXPENSIVE_STAT_TIMERS`) at the top of `src/core/stats.h`.

##### Problem: I get slower performance in the first run of a program

This is normal and it is best to always run a program at least twice if you are interested in the timer results. Pyston will cache some jitted code from previous runs.

### Benchmarks

The main benchmarks we use at the moment are in [https://github.com/dropbox/pyston-perf](https://github.com/dropbox/pyston-perf), which you should definitely clone (in the same directory as Pyston) and use.

To measure the performance impact of your changes, you will first need to get the latest baseline performance numbers on your machine. Compile `master` with `make pyston_release` and run

```
python ../pyston-perf/benchmarking/measure_perf.py --save baseline --run-times 3 --take-min
```

Then switch to your branch, recompile and compare with

```
python ../pyston-perf/benchmarking/measure_perf.py --compare baseline --run-times 3 --take-min
```

This will produce output that looks like

```
              pyston (calibration)                      :    1.0s baseline: 1.0 (-1.8%)
              pyston django_template.py                 :    4.6s baseline: 4.5 (+2.2%)
              pyston pyxl_bench.py                      :    3.7s baseline: 3.7 (+0.2%)
              pyston sqlalchemy_imperative2.py          :    5.2s baseline: 5.1 (+1.1%)
              pyston django_migrate.py                  :    1.7s baseline: 1.7 (+1.1%)
              pyston virtualenv_bench.py                :    5.3s baseline: 5.2 (+1.6%)
              pyston interp2.py                         :    3.9s baseline: 3.9 (-0.4%)
              pyston raytrace.py                        :    5.6s baseline: 5.4 (+4.1%)
              pyston nbody.py                           :    7.2s baseline: 6.9 (+5.1%)
              pyston fannkuch.py                        :    6.1s baseline: 5.8 (+5.6%)
              pyston chaos.py                           :   18.2s baseline: 17.3 (+5.2%)
              pyston fasta.py                           :    4.4s baseline: 4.1 (+7.8%)
              pyston pidigits.py                        :    5.5s baseline: 5.4 (+2.5%)
              pyston richards.py                        :    1.5s baseline: 1.5 (+0.6%)
              pyston deltablue.py                       :    1.4s baseline: 1.3 (+1.0%)
              pyston (geomean-10eb)                     :    4.3s baseline: 4.2 (+2.7%)
```

The numbers shown is the smallest value obtained after 3 (or more, if you wish) runs. It is recommended to include these outputs in non-trivial pull requests as a way to document the performance impacts, if any.

##### Problem: My trivial change just created a 2% reduction in performance

Unfortunately, variations in performance can be fairly large across runs, relatively speaking. The variance usually isn't more than 1-2%, but given the breadth of Python code, performance is can sometimes only be achieved via dozens of 0.5% optimizations and it is hard to measure a 0.5% performance change if the variance is 1%. So depending on the machine Pyston is running on, the variance can feel quite large. For small optimizations, it's useful to write a small microbenchmark (e.g. a tight loop).

To add to the challenge of precise performance measurements, *adding trivial code or even unreachable code can change the performance characteristics* by moving other hot functions around in the assembly which can help or hinder the effectiveness of the instruction cache. This is something we plan to address in the future, but reducing variance across runs would be a very valuable contribution if you are interested (e.g. by placing large gaps between assemblies of different functions and aligning them to cache lines).

Some benchmarks are particularly prone to variations caused by trivial changes. [http://speed.pyston.org/](http://speed.pyston.org/timeline) shows the performance graphs of many of the benchmarks. If a benchmark already shows a fair amount of up and downs, it is not as big a deal if the PR causes a performance drop in that benchmark. For example, [django_template](http://speed.pyston.org/timeline/#/?exe=1,3&base=none&ben=django_template&env=2&revs=1000&equid=off) is a benchmark that performs better and better, but [nbody](http://speed.pyston.org/timeline/#/?exe=1,3&base=none&ben=nbody&env=2&revs=1000&equid=off) has the tendency to be affected by the most unrelated changes.

### Profiling tools

The general go-to tool for profiling that we use is `perf`, which shows the hot functions in which the program spends the most time and their callers. It can be used as follows:

```
make perf_BASEFILENAME
```

which is essentially

```
perf record -g -- ./pyston_release -p -q BASEFILENAME
```

This will create a `.perf.data` file in the same directory that can be viewed using `perf report -n`. We recommend using the `-n` flag which shows the number of samples per function as well as the percentage, which is useful when comparing two perf runs.

The `-p -q` flags output more information about jitted frames that perf can use. Perf has a few other useful flags, such as `-e page-faults` which counts the number of page faults during an execution. While we optimize some of it away, Python intrinsically allocates massive amounts of memory and we are definitely at the point where it is worth thinking about caching, at all levels of abstraction.

For some notes on other profiling tools, see [PROFILING](PROFILING).

## Travis CI

We use Travis CI for continuous integration. Pull requests to the main repository will be run automatically against our full test suite upon creation and updates.

##### Problem: My code passes all the tests locally but fails on Travis CI

Travis CI runs the tests in release mode, so you may want to try `make check_release` or `make check_release ARGS=TESTNAME` in your local machine. If the Clang build works but not the GCC build, build with GCC using `make pyston_gcc`.

Travis CI runs on Ubuntu 12.04.5 LTS - having a similar setup would definitely help.

## Debugging

When Pyston crashes, the first thing you want to do is to get the crash location inside GDB. How do you continue from that point on?

Looking at the classes of objects in the current stack frame is always a good start. Any C++ type that inherits from `Box` (of the form `Box.*`) should have a `cls` field which stores a `BoxedClass`, the class of the object. You would want to use `p obj->cls->tp_name`. If the value looks like garbage, there is a good chance that the object has either been corrupted or that we can somehow access memory that has been freed. Although you won't see them directly in the C++ definition of `BoxedClass`, `BoxedClass`s have [all the fields that type objects in CPython do](https://docs.python.org/2/c-api/typeobj.html) (fields with the prefix `tp_`).

In general, boxes are allocated in the heap (but not all - e.g. classes of built-in types) in one of the three heaps. Pointers in the heap have a hardcoded range of allowed values (see `src/gc/heap.h`) - values that fall outside this range (and are not in the stack) could potentially indicate memory errors.

The C++ stacktrace can be quite difficult to read. Every Python function call will result in half a dozen C++ function calls along the lines of `getAttrInternal`. This is why using `signal SIGUSR1` which will call `_printStacktrace()` is useful as it gives you a more readeable with Python stacktrace (albeit with less information).

##### Problem: The tests fail in release mode

For example, `tests/sys_argv` might output `['/mnt/rudi/pyston/test/tests/sys_argv.py']` instead of `['/mnt/rudi/pyston/./test/tests/sys_argv.py']`. If you switch between testing in debug mode and release mode, try running `touch test/tester.py` in-between runs.

##### Problem: I can't reproduce a crash consistently on my machine

This can be caused by objects being allocated in different places in memory in each run and changing the way GC/memory errors manifest. Try disabling [Address Space Layout Randomization](http://docs.oracle.com/cd/E37670_01/E36387/html/ol_kernel_sec.html).

##### Problem: The integration tests crash

While we do our best to continuously update our test suite to cover more edge cases, bugs introduced by new changes are often uncovered only by large existing programs that do many things. Such tests reside in `test/integration/` and run existing Python libraries such as `pip`, `pyxl` or `sqlalchemy`. Unfortunately, these large integration tests have the tendency to span subprocesses which makes debugging much more challenging.

The test `virtualenv_test` is the most frequent test that uncovers new bugs, and will be used as an example to show the kind of debugging techniques that help get through this challenge. It may segfault after a new change, but `gdb --args ./pyston_dbg test/integration/virtualenv_test.py` won't break on the offending line because the segfault happens in one of the many subprocesses `virtualenv` spawns - the main script itself does very little.

You can try to use GDB's [follow-fork-mode](https://sourceware.org/gdb/onlinedocs/gdb/Forks.html) though I personally have had mixed levels of success using this technique.

The first step is to figure out which subprocess crash. For example, it could be the call to `pip install sqlalchemy==1.0.0`. If you can, try to run that command on its own (though you will have to do so in the `test_env` folder where the `pip` executable resides). If you are lucky, you can reproduce the crash. However, chances are that running the subprocess independently will work just fine - this could be, for example, because running the subprocess on it's own doesn't let Pyston run for long enough to start JITing code. In that case, you will want to change the script to run `gdb --args ./test_env/bin/pyston_dbg test_env/bin/pip install sqlalchemy=1.0.0`. This will run `pip` inside GDB and break on failure.

Unfortunately, running a subprocess inside GDB can magically cause the crash to disappear and appear in another subprocess. Trying to chase the crash can be quite time-consuming and it is often more effective to attach GDB to every subprocess. To avoid running `run` manually in GDB on every subprocess, the following options are useful: `--eval-command=run --eval-command="set disable-randomization on" --eval-command=quit`. This will automatically start the program inside GDB and quit unless a crash causes GDB to pause.

Sometimes, the offending subprocess is created using the Python subprocess libraries rather than from a string run as a shell script. For example, in `test/integration/virtualenv/virtualenv.py` in the function `install_wheel`:

```
    cmd = [
        py_executable, '-c',
        'import sys, pip; sys.exit(pip.main(["install", "--ignore-installed"] + sys.argv[1:]))',
    ] + project_names
    ...
    try:
        call_subprocess(cmd, show_stdout=False,
            extra_env = {
                'PYTHONPATH': pythonpath,
                'PIP_FIND_LINKS': findlinks,
                'PIP_USE_WHEEL': '1',
                'PIP_PRE': '1',
                'PIP_NO_INDEX': '1'
            }
        )
    finally:
        ...
```

You will want to wrap the command inside GDB

```
    cmd = [
        "gdb", "--args",
        py_executable, '-c',
        'import sys, pip; sys.exit(pip.main(["install", "--ignore-installed"] + sys.argv[1:]))',
    ] + project_names
```

and make sure that you can see the GDB prompt by changing `show_stdout=False` to `show_stdout=True`.

##### Problem: I still can't isolate the crashing subprocess

Wrapping a subprocess call in GDB can be quick and effective, but sometimes you run into a [heisenbug](https://en.wikipedia.org/wiki/Heisenbug) and GDB hides the problem no matter what you do.

If you can isolate the nature of the crash (e.g. segmentation fault), you may be able to add a signal handler with an infinite loop and attach GDB to the process after-the-fact by starting GDB and using `attach PID` where `PID` is the hanging `pyston_dbg` process id. You may want to look inside `src/codegen/entry.cpp` for signal handlers. If you want to pause on segfault, you may want to add a snippet of code like:

```
static void handle_sigsegv(int signum) {

    assert(signum == SIGSEGV);

    fprintf(stderr, "SIGSEGV, printing stack trace\n");

    _printStacktrace();

    printf("HANDLE SIGSEGV %d\n", getpid());

    fprintf(stderr, "HANDLE SIGSEGV %d\n", getpid());

    while (true) {
    }

}

void initCodegen() {
    ...
    signal(SIGSEGV, &handle_sigsegv);
    ...
}
```

##### Problem: The integration tests (especially virtualenv) crash inconsistently

Integration tests that run `pip` will often cache downloaded packages, so you may want to try `rm -rf ~/.cache/pip`. We do not clear the cache by default because the test takes longer to run and we run into cases where the failure can occur only when packages are cached just as often as when the packages are not cached.

##### Problem: I found the crash, but the stacktrace is convoluted

The real cause of an error is often deeper down in the stack. If an assertion failed for example, it may be the case that the assertion calls an exception handler, which tries unwinding the stack for debug information, then fails again and triggers another assertion, which is the one you see. If you see anything that refers to unwinding or exception handling in the stacktrace, you may need to look deeper down.

##### Problem: I get a segmentation due to stackoverflow from an infinite loop of mutually recursive functions like `runtimeCall`.

Pyston started without support for [CPython slots](https://docs.python.org/2/c-api/typeobj.html#sequence-structs) (e.g. `tp_` or `mp_`, `sq_`, etc) but they were introduced later. Some legacy code will still assign functions to the `tp_` slots that do an attribute lookup for a function that contains the logic instead of just running the logic directly. For example, `slot_tp_del` might do an attribute for `__del__` instead of just calling a finalizer.

Sometimes, that attribute lookup ends up calling the `tp_` slot again which creates a loop which only terminates with a stackoverflow. The solution in those cases is often to figure out the object and the slot involved in the recursive calls, and assign a non-default function to the `tp_` slot that does not do another attribute lookup.

##### Problem: My object is not getting garbage collected

This mostly concerns code or tests that involve finalizers and weak reference callbacks in some way.

There are many reason that an object might be retained longer than you would expect due to artifacts of the implementation. We scan the memory conservatively when we need to, such as the stack, like most other garbage-collected language implementations. That means that any value that looks like a pointer (see `src/gc/heap.cpp`) is assumed to be a pointer.

There are a lot of ways a pointer to an object could remain on the stack despite being conceptually unreachable. The pointer could have been placed on the stack when it was used as the argument to a function, but newer function calls never overwrote that value due to gaps in or between stack frames.

The pointer value could have also been generated randomly. How likely is it that an integer value just happens to be a valid pointer? More often than you would think, and for the most obscure reasons. For example, you have a valid pointer `0x127021df48` to some random object, such as an xrange iterator which the compiler stores in register `r14` as an optimization. Then, the register gets reused, but in such a way that the `mov` instruction operates only in the lower 8 bits of `r14` to store the value `1`. Now, `r14` contains the value `0x127021e001`, a valid interior pointer to an otherwise unreachable object. Then, `r14` gets pushed onto the stack by callee-save conventions. Now, a pointer to this object exists on the stack so it doesn't get freed until that stack frame is popped.

It can get really obscure.
