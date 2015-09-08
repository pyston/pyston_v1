These are rntz's notes from trying to get the CPython testing framework to run,
for future reference.

## Bugs uncovered
The CPython tests I've included fail for various reasons. Recurring issues include:
- use of `compile()`
- missing `__hash__` implementations for some builtin types
- we don't have `imp.get_magic()`
- segfaults
- missing `__name__` attributes on capifuncs
- missing `sys.__stdout__` attribute
- `serialize_ast.cpp`: `writeColOffset: assert(v < 100000 || v == -1)` gets tripped
- `pypa-parser.cpp`: readName: `assert(e.type == pypa::AstType::Name)`
- `src/runtime/util.cpp`: `parseSlice`: `assert(isSubclass(start->cls, int_cls) || start->cls == none_cls)`

## List of files & why they're failing
```
FILE                    REASONS
------------------------------------------------------
test_abc                [unknown]
test_bisect             somehow sys.modules['_bisect'] is getting set to 0
test_builtin            execfile scoping issue
test_class              needs ellipsis
test_coercion           1**1L, divmod(1, 1L); some unknown bug
test_collections        assertion failed when doing vars(collections.namedtuple('Point', 'x y')(11, 12))
test_contextlib         lock.locked attributes
test_datetime           needs _PyObject_GetDictPtr
test_decimal            I think we need to copy decimaltestdata from cpython
test_decorators         decorator bug -- we evaluate decorator obj and its args in wrong order
test_deque              couple unknown issues
test_descr              wontfix: crashes at "self.__dict__ = self"
test_descrtut           `exec in DefaultDict()`
test_dict               misc failures related to things like gc, abc, comparisons, detecting mutations during iterations
test_dictcomps          we need to disallow assigning to dictcomps
test_dictviews          various unique bugs
test_doctest            hard to know.  also missing some input files
test_enumerate          assert instead of exception in BoxedEnumerate
test_exceptions         we are missing recursion-depth checking
test_extcall            f(**kw) crashes if kw isn't a dict
test_file               wontfix: we don't destruct file objects when the test wants
test_file2k             we abort when you try to open() a directory
test_file_eintr         not sure
test_funcattrs          we don't allow changing numing of function defaults
test_functools          unknown errors
test_generators         crash when sending non-None to a just-started generator
test_genexps            parser not raising a SyntaxError when assigning to a genexp
test_global             SyntaxWarnings for global statements after uses
test_grammar            bug in our tokenizer
test_io                 memory/gc issue?
test_iterlen            [unknown]
test_itertools          [unknown]
test_json               'from test.script_helper import assert_python_ok' fails; sounds like it is trying to look at pycs
test_list               longs as slice indices
test_long               sys.long_info
test_module             unicode docstrings
test_mutants            unknown failure
test_optparse           assertion instead of exceptions for long("invalid number")
test_pep277             segfaults
test_pep352             various unique bugs
test_pkg                unknown bug
test_random             long("invalid number")
test_repr               complex.__hash__; some unknown issues
test_richcmp            PyObject_Not
test_scope              eval of code object from existing function (not currently supported)
test_set                weird function-picking issue
test_setcomps           parser not raising a SyntaxError when assigning to a setcomp
test_sort               argument specification issue in listSort?
test_str                memory leak?
test_string             infinite loops in test_replace
test_subprocess         exit code 141 [sigpipe?], no error message
test_types              PyErr_WarnEx
test_undocumented_details   function.func_closure
test_unicode            argument passing issue?
test_unicode_file       exit code 139, no error message
test_unittest           serialize_ast assert
test_urllib2            segfault due to attrwrapper corruption
test_userdict           segfault: repr of recursive dict?
test_userlist           slice(1L, 1L)
test_userstring         float(1L); hangs in test_replace
test_uuid               long("invalid number")
test_weakref            weird function-picking bug (something around float.__add__)
test_weakset            unknown issues
test_with               weird codegen assert
test_wsgiref            unknown issue
test_xrange             unknown type analysis issue
```

### Getting regrtest to work is hard
regrtest works by `__import__()ing` the tests to be run and then doing some stuff.
This means that if even a single test causes pyston to crash or assert(), the
whole test-runner dies.

The best fix for this would be to simply run each test in a separate pyston
process. It's not clear to accomplish this without breaking the tests, however,
because Python's test support is a big, gnarly piece of code. In particular, it
will skip tests based on various criteria, which we probably want to support.
But it's not clear how to disentangle that knowledge from the way it `__import__`s
the tests.

So instead I ran regrtest manually, looked at what tests it ran, and imported
those. tester.py will run them separately.

### Obsolete notes: Hacking regrtest to manually change directories
Need to run test/regrtest.py for testing harness; The standard way to do this in
CPython is `python -m test.regrtest` from Lib/. The -m is important because
otherwise it can't find the tests properly. Turns out implementing the -m flag
is hard, because the machinery for imports is pretty complicated and there's no
easy way to ask it "which file *would* I load to import this module". So I
hacked regrtest to manually change directories.

### Obsolete FIXME for regrtest.py: CFG/analysis bug with osr
test/regrtest.py trips an assert in PropagatingTypeAnalysis::getTypeAtBlockStart
if not run with -I, looks like malformed CFG or bug in analysis
* tests are slow
CPython's tests are pretty slow for us. In particular, since we're not running
with regrtest, we have to go through the set-up time of loading
test.test_support for each test. On my machine it's that's about a half-second
per test.

To handle this, by default we don't run tests marked "expected: fail". To
disable this, pass --all-cpython-tests to tester.py.
