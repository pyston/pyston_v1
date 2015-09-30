import gc
import os
import sys
import subprocess
import traceback

ENV_NAME = os.path.abspath("sqlalchemy_test_env_" + os.path.basename(sys.executable))

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib//virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)

        subprocess.check_call([ENV_NAME + "/bin/pip", "install", "mock==1.0.0", "pytest==2.7.2", "py==1.4.30"])
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]

# subprocess.check_call([os.path.abspath("sqlalchemy_test_env/bin/python"), "-c", "import py; print type(py); print py.builtin"])

SQLALCHEMY_DIR = os.path.abspath(os.path.dirname(__file__) + "/../lib/sqlalchemy")
TEST_DIR = SQLALCHEMY_DIR + "/test"
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

sys.path.append(SQLALCHEMY_DIR + "/lib")
sys.path.insert(0, SQLALCHEMY_DIR)
sys.path.append(ENV_NAME + "/site-packages")
sys.path.append(ENV_NAME + "/lib/python2.7/site-packages")

os.chdir(SQLALCHEMY_DIR)


# make sure this is importable:
import mock

# sqlalchemy has a bad implementation of is_cpython (in compat.py): it's "not pypy and not is jython".
# Monkey-patch in a properly-detected value (it uses this to gate some "requires a predictable gc" tests
# we fail):
import platform
import sqlalchemy.util.compat
sqlalchemy.util.compat.cpython = sqlalchemy.util.cpython = (platform.python_implementation() == "CPython")

import sqlalchemy.testing
import sqlalchemy.testing.plugin.pytestplugin
class Options(object):
    pass
options = Options()
options.__dict__.update({'noassert': False, 'verbose': 1, 'color': 'auto', 'collectonly': False, 'pyargs': False, 'pastebin': None, 'genscript': None, 'include_tag': None, 'plugins': [], 'dbs': None, 'log_debug': None, 'markexpr': '', 'help': False, 'capture': 'fd', 'low_connections': False, 'requirements': None, 'reportchars': 'fxX', 'reversetop': False, 'assertmode': 'rewrite', 'backend_only': False, 'markers': False, 'strict': False, 'usepdb': False, 'inifilename': None, 'version': False, 'log_info': None, 'dropfirst': False, 'maxfail': 25, 'traceconfig': False, 'junitprefix': None, 'force_write_profiles': False, 'durations': None, 'db': None, 'confcutdir': None, 'doctestmodules': False, 'showfixtures': False, 'fulltrace': False, 'file_or_dir': ['/mnt/kmod/pyston/test/lib/sqlalchemy'], 'basetemp': None, 'report': None, 'ignore': None, 'exclude_tag': None, 'resultlog': None, 'doctestglob': 'test*.txt', 'dburi': None, 'exitfirst': False, 'showlocals': False, 'keyword': '', 'doctest_ignore_import_errors': False, 'write_profiles': False, 'runxfail': False, 'quiet': 0, 'cdecimal': False, 'xmlpath': None, 'tbstyle': 'native', 'debug': False, 'nomagic': False})
class SkipTest(Exception):
    pass
sqlalchemy.testing.plugin.plugin_base.set_skip_test(SkipTest)
sqlalchemy.testing.plugin.plugin_base.pre_begin(options)
sqlalchemy.testing.plugin.plugin_base.read_config()
sqlalchemy.testing.plugin.pytestplugin.pytest_sessionstart(None)

# Bug in this file: they forgot to import SkipTest
import sqlalchemy.testing.profiling
sqlalchemy.testing.profiling.SkipTest = SkipTest

# The tests are kind enough to call `lazy_gc()` to make sure things get finalized,
# but with our conservative collector that's usually not enough.
import sqlalchemy.testing.util
def no_gc():
    raise SkipTest()
sqlalchemy.testing.util.lazy_gc = no_gc

import glob
test_files = glob.glob(TEST_DIR + "/test*.py") + glob.glob(TEST_DIR + "/*/test*.py")
test_files.sort()

CPYTHON_PASSING = [
    'test.aaa_profiling.test_compiler', 'test.aaa_profiling.test_memusage',
    'test.aaa_profiling.test_orm', 'test.aaa_profiling.test_pool', 'test.aaa_profiling.test_resultset',

    'test.base.test_dependency', 'test.base.test_events', 'test.base.test_except', 'test.base.test_inspect', 'test.base.test_utils',

    'test.dialect.test_mxodbc', 'test.dialect.test_pyodbc', 'test.dialect.test_sqlite', 'test.dialect.test_sybase',

    'test.engine.test_bind', 'test.engine.test_ddlevents', 'test.engine.test_logging',
    'test.engine.test_parseconnect', 'test.engine.test_pool', 'test.engine.test_reconnect',

    'test.ext.test_compiler', 'test.ext.test_extendedattr', 'test.ext.test_hybrid', 'test.ext.test_orderinglist',

    'test.orm.test_association', 'test.orm.test_assorted_eager', 'test.orm.test_attributes', 'test.orm.test_backref_mutations',
    'test.orm.test_bind', 'test.orm.test_bulk', 'test.orm.test_bundle', 'test.orm.test_collection',
    'test.orm.test_compile', 'test.orm.test_composites', 'test.orm.test_cycles', 'test.orm.test_defaults',
    'test.orm.test_default_strategies', 'test.orm.test_deferred', 'test.orm.test_deprecations', 'test.orm.test_descriptor',
    'test.orm.test_dynamic', 'test.orm.test_eager_relations', 'test.orm.test_evaluator', 'test.orm.test_events',
    'test.orm.test_expire', 'test.orm.test_hasparent', 'test.orm.test_immediate_load', 'test.orm.test_inspect',
    'test.orm.test_joins', 'test.orm.test_lazy_relations', 'test.orm.test_load_on_fks', 'test.orm.test_lockmode',
    'test.orm.test_manytomany', 'test.orm.test_merge', 'test.orm.test_naturalpks', 'test.orm.test_of_type',
    'test.orm.test_onetoone', 'test.orm.test_options', 'test.orm.test_query', 'test.orm.test_relationships',
    'test.orm.test_rel_fn', 'test.orm.test_scoping', 'test.orm.test_selectable', 'test.orm.test_session',
    'test.orm.test_sync', 'test.orm.test_transaction', 'test.orm.test_unitofworkv2', 'test.orm.test_update_delete',
    'test.orm.test_utils', 'test.orm.test_validators', 'test.orm.test_versioning',

    'test.sql.test_case_statement', 'test.sql.test_compiler', 'test.sql.test_constraints', 'test.sql.test_cte',
    'test.sql.test_ddlemit', 'test.sql.test_delete', 'test.sql.test_functions', 'test.sql.test_generative',
    'test.sql.test_insert', 'test.sql.test_inspect', 'test.sql.test_join_rewriting', 'test.sql.test_labels',
    'test.sql.test_operators', 'test.sql.test_query', 'test.sql.test_quote', 'test.sql.test_rowcount',
    'test.sql.test_selectable', 'test.sql.test_text', 'test.sql.test_unicode',
]

# These are the ones that pass on CPython (ie that we've stubbed enough of their testing
# infrastructure to run):
MODULES_TO_TEST = [
    'test.aaa_profiling.test_compiler',
    'test.aaa_profiling.test_orm',
    'test.aaa_profiling.test_pool',
    'test.base.test_dependency',
    'test.base.test_events',
    'test.base.test_except',
    'test.base.test_inspect',
    'test.base.test_utils',
    'test.dialect.test_mxodbc',
    'test.dialect.test_pyodbc',
    'test.dialect.test_sybase',
    'test.engine.test_bind',
    'test.engine.test_ddlevents',
    'test.engine.test_logging',
    'test.engine.test_parseconnect',
    'test.engine.test_pool',
    'test.engine.test_reconnect',
    'test.ext.test_compiler',
    'test.ext.test_hybrid',
    'test.ext.test_orderinglist',
    'test.orm.test_association',
    'test.orm.test_assorted_eager',
    'test.orm.test_attributes',
    'test.orm.test_backref_mutations',
    'test.orm.test_bind',
    'test.orm.test_bulk',
    'test.orm.test_bundle',
    'test.orm.test_collection',
    'test.orm.test_compile',
    'test.orm.test_composites',
    'test.orm.test_cycles',
    'test.orm.test_defaults',
    'test.orm.test_default_strategies',
    'test.orm.test_deferred',
    'test.orm.test_deprecations',
    'test.orm.test_descriptor',
    'test.orm.test_eager_relations',
    'test.orm.test_evaluator',
    'test.orm.test_events',
    'test.orm.test_expire',
    'test.orm.test_hasparent',
    'test.orm.test_immediate_load',
    'test.orm.test_inspect',
    'test.orm.test_joins',
    'test.orm.test_lazy_relations',
    'test.orm.test_load_on_fks',
    'test.orm.test_lockmode',
    'test.orm.test_manytomany',
    'test.orm.test_naturalpks',
    'test.orm.test_of_type',
    'test.orm.test_onetoone',
    'test.orm.test_options',
    'test.orm.test_query',
    'test.orm.test_rel_fn',
    'test.orm.test_scoping',
    'test.orm.test_selectable',
    'test.orm.test_sync',
    'test.orm.test_transaction',
    'test.orm.test_unitofworkv2',
    'test.orm.test_update_delete',
    'test.orm.test_utils',
    'test.orm.test_validators',
    'test.sql.test_case_statement',
    'test.sql.test_constraints',
    'test.sql.test_cte',
    'test.sql.test_ddlemit',
    'test.sql.test_delete',
    'test.sql.test_functions',
    'test.sql.test_generative',
    'test.sql.test_insert',
    'test.sql.test_inspect',
    'test.sql.test_join_rewriting',
    'test.sql.test_labels',
    'test.sql.test_operators',
    'test.sql.test_query',
    'test.sql.test_rowcount',
    'test.sql.test_selectable',
    'test.sql.test_text',
]

FAILING = [
    # 'test.aaa_profiling.test_memusage',   # Wants gc.get_objects
    # 'test.aaa_profiling.test_resultset',  # Wants sys.getrefcount
    # 'test.dialect.test_sqlite',           # ascii codec can't encode
    # 'test.ext.test_extendedattr',         # does `locals()[42] = 99` in a classdef to prove it can.  maybe we could say is_pypy to avoid it.
    # 'test.orm.test_dynamic',              # not sure; things end up being put in tuples
    # 'test.orm.test_merge',                # needs PyObject_AsWriteBuffer
    # 'test.orm.test_relationships',        # not sure; things end up being put in tuples
    # 'test.orm.test_session',              # unclear
    # 'test.orm.test_versioning',           # crashes in the uuid module with an AttributeError from ctypes
    # 'test.sql.test_compiler',             # unclear
    # 'test.sql.test_quote',                # unclear
    # 'test.sql.test_unicode',              # "ascii codec can't encod character"
]

# MODULES_TO_TEST = ['test.orm.test_bulk']
# MODULES_TO_TEST = FAILING[:1]

passed = []
failed = []

for run_idx in xrange(1):
  for fn in test_files:
    assert fn.startswith(TEST_DIR + '/')
    assert fn.endswith(".py")
    mname = fn[len(SQLALCHEMY_DIR) + 1:-3].replace('/', '.')
    if mname not in MODULES_TO_TEST:
        continue

    if mname == 'test.sql.test_functions' and run_idx > 0:
        continue

    print '=' * 50
    print mname

    try:
        m = __import__(mname, fromlist=["__all__"])
        for clsname in dir(m):
            cls = getattr(m, clsname)
            if clsname.startswith('_') or not clsname.endswith("Test") or not isinstance(cls, type):
                continue
            print "Running", cls

            if hasattr(cls, "setup_class"):
                cls.setup_class()
            n = cls()
            for t in dir(n):
                if not t.startswith("test_"):
                    continue

                # These test should be marked as requiring predictable_pc
                if (clsname == "SubclassGrowthTest" and t == "test_subclass"):
                    continue

                print "Running", t
                n.setup()
                try:
                    getattr(n, t)()
                except SkipTest:
                    pass
                n.teardown()
            if hasattr(cls, "teardown_class"):
                cls.teardown_class()
            gc.collect()
    except Exception:
        print mname, "FAILED"
        traceback.print_exc()
        failed.append(mname)
    else:
        print mname, "PASSED"
        passed.append(mname)

print "passing:", passed
print "failing:", failed

print
if failed:
    print "FAILED"
    sys.exit(1)
else:
    print "PASSED"
