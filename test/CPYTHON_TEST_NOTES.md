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
- `src/runtime/util.cpp`: `parseSlice`: `assert(isSubclass(start->cls, int_cls) || start->cls == none_cls)`

## List of files & why they're failing
```
FILE                    REASONS
------------------------------------------------------
test_aepack             No module named aetypes
test_aifc               Unsupported subclassing from file?
test_al                 No module named al
test_applesingle        Not really a failure, but it tries to skip itself and we don't support that
test_audioop            No module named audioop
test_bigmem             not sure
test_bisect             `sys.modules['foo'] = 0; import foo; print foo' should print "0"
test_bsddb185           No module named _bsddb185
test_bsddb3             No module named _bsddb
test_bsddb              No module named _bsddb
test_builtin            Can't use unicode attribute names; leaked refs
test_capi               No module named _testcapi
test_cd                 No module named cd
test_class              "exec foo in <old style class>.__dict__"
test_cl                 No module named cl
test_cmd_line_script    A number of issues
test_cmd_line           Missing a number of command line flags
test_codecencodings_cn  [unknown]
test_codecencodings_hk  [unknown]
test_codecencodings_iso2022  [unknown]
test_codecencodings_jp  [unknown]
test_codecencodings_kr  [unknown]
test_codecencodings_tw  [unknown]
test_codecmaps_cn       [unknown]
test_codecmaps_hk       [unknown]
test_codecmaps_jp       [unknown]
test_codecmaps_kr       [unknown]
test_codecmaps_tw       [unknown]
test_codecs             Missing _codecs_tw, maybe others
test_codeop             code.__eq__?
test_code               'code' object has no attribute 'co_names'
test_coercion           1**1L, divmod(1, 1L); some unknown bug
test_compileall         Not sure if this test makes sense for us (wants to check the details of pyc files)
test_compiler           "import compiler" fails
test_compile            Missing some code-object fields
test_cprofile           [unknown]
test_crypt              No module named crypt
test_ctypes             No module named _testcapi
test_curses             No module named _curses_panel
test_datetime           Wants _PyObject_GetDictPtr for hcattrs
test_dbm                No module named dbm
test_decorators         callattr issue: we evaluate the args first then do the callattr, but if the getattr has side-effects this is the wrong order
test_descrtut           `exec in DefaultDict()`
test_descr              wontfix: crashes at "self.__dict__ = self"
test_dict               misc failures related to things like gc, abc, comparisons, detecting mutations during iterations
test_dictviews          segfault calling repr on recursive dictview. remove test/tests/test_dictview.py when the orig test passes
test_distutils          Doesn't like our .pyston.so extension; we need to copy in wininst-6.0.exe and similar
test_dis                dis not really supported in Pyston
test_dl                 No module named dl
test_doctest            hard to know.  also missing some input files
test_email_codecs       Missing 'euc-jp' codec, maybe others
test_email_renamed      crlf_separation failing
test_email              crlf_separation failing, missing codecs
test_enumerate          assert instead of exception in BoxedEnumerate
test_exceptions         we are missing recursion-depth checking
test_extcall            f(**kw) crashes if kw isn't a dict
test_frozen             "No module named __hello__"
test_funcattrs          we don't allow changing numing of function defaults
test_future             missing error messages
test_gc                 Wrong number of gc collections, gc.is_tracked issue?
test_gdbm               No module named gdbm
test_gdb                sysconfig missing PY_CFLAGS var
test_generators         crash when sending non-None to a just-started generator
test_genexps            parser not raising a SyntaxError when assigning to a genexp
test_getargs2           No module named _testcapi
test_global             SyntaxWarnings for global statements after uses
test_gl                 No module named gl
test_grammar            bug in our tokenizer
test_hotshot            No module named _hotshot
test_idle               No module named Tkinter
test_imageop            No module named imageop
test_imgfile            no module named imgfile
test_importhooks        "import compiler" doesnt work
test_import             Marshaling code objects not supported
test_inspect            missing sys.exc_traceback
test_io                 memory/gc issue?
test_iterlen            [unknown]
test_itertools          range(sys.maxint-5, sys.maxint+5)
test_json               "throw called on generator last advanced with __hasnext__"
test_kqueue             Not really a failure, but it tries to skip itself and we don't support that
test_linuxaudiodev      No module named audioop
test_list               longs as slice indices
test_long_future        Rounding issues for long.__truediv__ (we convert to doubles then round; should do a full-precision division and then round)
test_macos              Not really a failure, but it tries to skip itself and we don't support that
test_macostools         Not really a failure, but it tries to skip itself and we don't support that
test_marshal            Can't marshal code objects
test_modulefinder       Wants to scan code objects
test_module             unicode docstrings
test_msilib             No module named _msi
test_multibytecodec     No module named _multibytecodec
test_mutants            unknown failure
test_new                Tries `new.function(f.func_code, {}, "blah")` on a code that didn't expect custom globals
test_nis                "no module named nis"
test_optparse           assertion instead of exceptions for long("invalid number")
test_ossaudiodev        [unknown]
test_pdb                [unknown]
test_peepholer          tries to disassemble code objects
test_pep352             various unique bugs
test_pkg                we don't import quite the right names for "import *"?
test_pprint             Dict ordering, some other issues
test_profile            sys has no attribute setprofile
test_py3kwarn           [unknown]
test_pyclbr             This test passes but takes a very long time in debug mode (60s vs 5s for release mode).
test_pydoc              Not sure, not generating the right docstrings
test_random             long("invalid number")
test_repr               complex.__hash__; some unknown issues
test_resource           fails on travis-ci: setrlimit RLIMIT_CPU not allowed to raise maximum limit
test_runpy              recursion-depth sisues on debug, 'imp has no attribute "get_magic"'
test_scope              eval of code object from existing function (not currently supported)
test_scriptpackages     No module named aetools
test_site               We're missing the '-s' command line flag
test_startfile          Only works on windows
test_str                A few misc errors
test_structmembers      No module named _testcapi
test_subprocess         exit code 141 [sigpipe?], no error message
test_sunaudiodev        No module named sunaudiodev
test_sunau              No module named audioop, some other issues
test_symtable           No module named _symtable
test_syntax             We're missing "too many statically nested blocks" check; a couple error messages don't look right to the tests
test_sys_setprofile     Not supported yet in pyston
test_sys_settrace       Not supported yet in pyston
test_sys                we're missing some attributes in the sys module (call_tracing, __excepthook__, setrecursionlimit, etc)
test_tcl                No module named _tkinter
test_threading          Multiple issues, including not having sys.settrace
test_tk                 No module named _tkinter
test_tools              Skips itself because it's an "installed python"
test_traceback          Missing sys.exc_traceback
test_trace              sys.settrace not supported yet
test_transformer        "import compiler" not supported yet
test_ttk_guionly        No module named _tkinter
test_ttk_textonly       No module named _tkinter
test_undocumented_details   function.func_closure
test_unicode            argument passing issue?
test_userdict           segfault: repr of recursive dict?
test_userlist           slice(1L, 1L)
test_userstring         float(1L); hangs in test_replace
test_warnings           Among other things, we don't support the -W flag
test_weakref            weird function-picking bug (something around float.__add__), plase also fix the weakref issue in test_abc
test_winreg             No module named _winreg
test_winsound           No module named winsound
test_xml_etree_c        _elementtree.c does a bunch of CAPI calls without checking for exceptions, which ends up tripping some asserts
test_xml_etree          Missing sys.exc_value, unknown encoding "gbk"
test_xmlrpc             Cannot re-init internal module sys (`import sys; del sys.modules['sys']; import sys`)
test_zipfile64          [unknown]
test_zipimport_support  leaks; missing sys.settrace
test_zipimport          Marshalling of code objects not supported
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
