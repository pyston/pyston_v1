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
test_aepack             No module named aetypes
test_aifc               Unsupported subclassing from file?
test_al                 No module named al
test_applesingle        Not really a failure, but it tries to skip itself and we don't support that
test_argparse           [unknown]
test_array              [unknown]
test_ascii_formattd     [unknown]
test_ast                [unknown]
test_asynchat           [unknown]
test_asyncore           [unknown]
test_atexit             [unknown]
test_audioop            [unknown]
test_bigmem             [unknown]
test_bisect             somehow sys.modules['_bisect'] is getting set to 0
test_bsddb185           [unknown]
test_bsddb3             [unknown]
test_bsddb              [unknown]
test_builtin            execfile scoping issue
test_bz2                [unknown]
test_capi               [unknown]
test_cd                 [unknown]
test_cfgparser          [unknown]
test_cgi                [unknown]
test_class              needs ellipsis
test_cl                 [unknown]
test_cmath              [unknown]
test_cmd_line_script    [unknown]
test_cmd_line           [unknown]
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
test_codecs             [unknown]
test_codeop             [unknown]
test_code               [unknown]
test_coding             [unknown]
test_coercion           1**1L, divmod(1, 1L); some unknown bug
test_collections        assertion failed when doing vars(collections.namedtuple('Point', 'x y')(11, 12))
test_compileall         [unknown]
test_compiler           [unknown]
test_compile            [unknown]
test_cookie             [unknown]
test_copy               Please debug this test in VM.
test_cpickle            [unknown]
test_cprofile           [unknown]
test_crypt              [unknown]
test_csv                [unknown]
test_ctypes             [unknown]
test_curses             [unknown]
test_datetime           needs _PyObject_GetDictPtr
test_dbm                [unknown]
test_decimal            I think we need to copy decimaltestdata from cpython
test_decorators         decorator bug -- we evaluate decorator obj and its args in wrong order
test_deque              couple unknown issues
test_descrtut           `exec in DefaultDict()`
test_descr              wontfix: crashes at "self.__dict__ = self"
test_dictcomps          we need to disallow assigning to dictcomps
test_dict               misc failures related to things like gc, abc, comparisons, detecting mutations during iterations
test_dictviews          various unique bugs
test_difflib            [unknown]
test_distutils          [unknown]
test_dis                [unknown]
test_dl                 [unknown]
test_doctest            hard to know.  also missing some input files
test_docxmlrpc          [unknown]
test_dumbdbm            [unknown]
test_email_codecs       [unknown]
test_email_renamed      [unknown]
test_email              [unknown]
test_enumerate          assert instead of exception in BoxedEnumerate
test_eof                [unknown]
test_exceptions         we are missing recursion-depth checking
test_extcall            f(**kw) crashes if kw isn't a dict
test_file2k             we abort when you try to open() a directory
test_file_eintr         not sure
test_fileio             [unknown]
test_file               wontfix: we don't destruct file objects when the test wants
test_fork1              [unknown]
test_fractions          [unknown]
test_frozen             [unknown]
test_ftplib             [unknown]
test_funcattrs          we don't allow changing numing of function defaults
test_functools          unknown errors
test_future5            [unknown]
test_future             [unknown]
test_gc                 [unknown]
test_gdbm               [unknown]
test_gdb                [unknown]
test_generators         crash when sending non-None to a just-started generator
test_genexps            parser not raising a SyntaxError when assigning to a genexp
test_getargs2           [unknown]
test_global             SyntaxWarnings for global statements after uses
test_gl                 [unknown]
test_grammar            bug in our tokenizer
test_hashlib            [unknown]
test_heapq              [unknown]
test_hotshot            [unknown]
test_httplib            [unknown]
test_httpservers        [unknown]
test_idle               [unknown]
test_imageop            [unknown]
test_imaplib            [unknown]
test_imgfile            [unknown]
test_importhooks        [unknown]
test_import             [unknown]
test_imp                [unknown]
test_inspect            [unknown]
test_io                 memory/gc issue?
test_iterlen            [unknown]
test_itertools          [unknown]
test_json               'from test.script_helper import assert_python_ok' fails; sounds like it is trying to look at pycs
test_kqueue             Not really a failure, but it tries to skip itself and we don't support that
test_lib2to3            [unknown]
test_linuxaudiodev      [unknown]
test_list               longs as slice indices
test__locale            No module named _locale
test_locale             [unknown]
test_longexp            [unknown]
test_long_future        [unknown]
test_long               sys.long_info
test_macos              Not really a failure, but it tries to skip itself and we don't support that
test_macostools         Not really a failure, but it tries to skip itself and we don't support that
test_mailbox            [unknown]
test_marshal            [unknown]
test_memoryio           [unknown]
test_memoryview         [unknown]
test_minidom            [unknown]
test_modulefinder       [unknown]
test_module             unicode docstrings
test_msilib             [unknown]
test_multibytecodec     [unknown]
test_multiprocessing    [unknown]
test_mutants            unknown failure
test_new                [unknown]
test_nis                [unknown]
test_old_mailbox        [unknown]
test_optparse           assertion instead of exceptions for long("invalid number")
test_ossaudiodev        [unknown]
test_os                 [unknown]
test_parser             [unknown]
test_pdb                [unknown]
test_peepholer          [unknown]
test_pep263             [unknown]
test_pep277             segfaults
test_pep352             various unique bugs
test_pickletools        [unknown]
test_pickle             unknown
test_pkg                unknown bug
test_poll               [unknown]
test_poplib             [unknown]
test_pprint             [unknown]
test_print              [unknown]
test_profile            [unknown]
test_py3kwarn           [unknown]
test_pyclbr             [unknown]
test_py_compile         [unknown]
test_pydoc              [unknown]
test_random             long("invalid number")
test_repr               complex.__hash__; some unknown issues
test_resource           [unknown]
test_richcmp            PyObject_Not
test_rlcompleter        [unknown]
test_runpy              [unknown]
test_sax                [unknown]
test_scope              eval of code object from existing function (not currently supported)
test_scriptpackages     [unknown]
test_set                lots of set issues
test_shelve             [unknown]
test_shlex              [unknown]
test_signal             [unknown]
test_site               [unknown]
test_smtplib            [unknown]
test_smtpnet            [unknown]
test_socketserver       [unknown]
test_socket             [unknown]
test_softspace          [unknown]
test_sort               argument specification issue in listSort?
test_sqlite             [unknown]
test_ssl                [unknown]
test_startfile          [unknown]
test_string             infinite loops in test_replace
test_str                memory leak?
test_strtod             [unknown]
test_structmembers      [unknown]
test_struct             [unknown]
test_subprocess         exit code 141 [sigpipe?], no error message
test_sunaudiodev        [unknown]
test_sunau              [unknown]
test_sundry             [unknown]
test_support            [unknown]
test_symtable           [unknown]
test_syntax             [unknown]
test_sysconfig          [unknown]
test_sys_setprofile     [unknown]
test_sys_settrace       [unknown]
test_sys                [unknown]
test_tarfile            [unknown]
test_tcl                [unknown]
test_telnetlib          [unknown]
test_tempfile           [unknown]
test_threaded_import    [unknown]
test_threadedtempfile   [unknown]
test_threading_local    [unknown]
test_threading          [unknown]
test_threadsignals      [unknown]
test_thread             [unknown]
test_timeout            [unknown]
test_tk                 [unknown]
test_tokenize           [unknown]
test_tools              [unknown]
test_traceback          [unknown]
test_trace              [unknown]
test_transformer        [unknown]
test_ttk_guionly        [unknown]
test_ttk_textonly       [unknown]
test_types              PyErr_WarnEx
test_undocumented_details   function.func_closure
test_unicode            argument passing issue?
test_unicodedata        [unknown]
test_unicode_file       exit code 139, no error message
test_unittest           serialize_ast assert
test_univnewlines2k     [unknown]
test_univnewlines       [unknown]
test_urllib2_localnet   [unknown]
test_urllib2net         [unknown]
test_urllib2            segfault due to attrwrapper corruption
test_urllibnet          [unknown]
test_userdict           segfault: repr of recursive dict?
test_userlist           slice(1L, 1L)
test_userstring         float(1L); hangs in test_replace
test_uuid               long("invalid number")
test_wait3              [unknown]
test_wait4              [unknown]
test_warnings           [unknown]
test_wave               [unknown]
test_weakref            weird function-picking bug (something around float.__add__)
test_weakset            unknown issues
test_winreg             [unknown]
test_winsound           [unknown]
test_with               weird codegen assert
test_wsgiref            unknown issue
test_xml_etree_c        [unknown]
test_xml_etree          [unknown]
test_xmlrpc             [unknown]
test_xpickle            [unknown]
test_xrange             unknown type analysis issue
test_zipfile64          [unknown]
test_zipfile            [unknown]
test_zipimport_support  [unknown]
test_zipimport          [unknown]
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
