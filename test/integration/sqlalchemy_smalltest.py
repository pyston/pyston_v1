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


import glob
test_files = glob.glob(TEST_DIR + "/test*.py") + glob.glob(TEST_DIR + "/*/test*.py")

# These are the ones that pass on CPython (ie that we've stubbed enough of their testing
# infrastructure to run):
MODULES_TO_TEST = [
    'test.base.test_dependency',
    'test.base.test_events',
    'test.base.test_except',
    'test.base.test_inspect',
    'test.base.test_utils',
    'test.dialect.test_mxodbc',
    'test.dialect.test_pyodbc',
    'test.dialect.test_sybase',
    'test.engine.test_parseconnect',
    'test.ext.test_compiler',
    'test.orm.test_descriptor',
    'test.orm.test_inspect',
    'test.orm.test_query',
    'test.sql.test_cte',
    'test.sql.test_ddlemit',
    'test.sql.test_inspect',
    'test.sql.test_operators',
]

passed = []
failed = []

for fn in test_files:
    assert fn.startswith(TEST_DIR + '/')
    assert fn.endswith(".py")
    mname = fn[len(SQLALCHEMY_DIR) + 1:-3].replace('/', '.')
    if mname not in MODULES_TO_TEST:
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
                if clsname == "SubclassGrowthTest" and t == "test_subclass":
                    # This test should be marked as requiring predictable_pc
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
