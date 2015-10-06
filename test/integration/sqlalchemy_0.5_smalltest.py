import gc
import os
import sys
import subprocess
import traceback

ENV_NAME = os.path.abspath("sqlalchemy_0_5_test_env_" + os.path.basename(sys.executable))

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
# if True:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib//virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)

        subprocess.check_call([ENV_NAME + "/bin/pip", "install", "mock==1.0.0", "nose", "py==1.4.30"])
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]

# subprocess.check_call([os.path.abspath("sqlalchemy_test_env/bin/python"), "-c", "import py; print type(py); print py.builtin"])

SQLALCHEMY_DIR = os.path.abspath(os.path.dirname(__file__) + "/../lib/sqlalchemy_0.5")
TEST_DIR = SQLALCHEMY_DIR + "/test"
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

sys.path.append(SQLALCHEMY_DIR + "/lib")
sys.path.insert(0, SQLALCHEMY_DIR)
sys.path.append(ENV_NAME + "/site-packages")
sys.path.append(ENV_NAME + "/lib/python2.7/site-packages")

os.chdir(SQLALCHEMY_DIR)


class Options(object):
    pass
options = Options()
options.__dict__.update({'cover_tests': None, 'enable_plugin_allmodules': None, 'multiprocess_restartworker': False, 'loggingConfig': None, 'doctestExtension': None, 'doctest_tests': None, 'enable_plugin_xunit': None, 'debugBoth': False, 'logcapture_clear': False, 'truthless': False, 'stopOnError': False, 'enable_plugin_id': None, 'testNames': None, 'doctestOptions': None, 'exclude': [], 'byteCompile': True, 'log_debug': None, 'ignoreFiles': [], 'logcapture': True, 'tableopts': [], 'capture': True, 'xunit_testsuite_name': 'nosetests', 'logcapture_level': 'NOTSET', 'noncomparable': False, 'py3where': None, 'noSkip': False, 'enable_plugin_isolation': None, 'logcapture_filters': None, 'collect_only': None, 'failed': False, 'version': False, 'mockpool': None, 'eval_attr': None, 'log_info': None, 'dropfirst': None, 'include': [], 'cover_xml': None, 'enable_plugin_profile': None, 'debugErrors': False, 'files': None, 'multiprocess_timeout': 10, 'logcapture_format': '%(name)s: %(levelname)s: %(message)s', 'testMatch': '(?:^|[\\b_\\./-])[Tt]est', 'enable_plugin_sqlalchemy': True, 'traverseNamespace': False, 'reversetop': False, 'firstPackageWins': False, 'db': 'sqlite', 'require': [], 'cover_branches': None, 'xunit_file': 'nosetests.xml', 'noDeprecated': False, 'cover_xml_file': 'coverage.xml', 'showPlugins': False, 'cover_erase': None, 'multiprocess_workers': 0, 'mysql_engine': None, 'addPaths': True, 'testIdFile': '.noseids', 'dburi': None, 'enable_plugin_coverage': None, 'attr': None, 'profile_sort': 'cumulative', 'doctestFixtures': None, 'logcapture_datefmt': None, 'cover_packages': None, 'verbosity': 1, 'enable_plugin_doctest': None, 'profile_stats_file': None, 'cover_inclusive': None, 'includeExe': False, 'unhashable': False, 'debugFailures': False, 'cover_html': None, 'detailedErrors': None, 'debugLog': None, 'doctest_result_var': None, 'enginestrategy': None, 'debug': None, 'cover_html_dir': 'cover', 'cover_min_percentage': None, 'where': None, 'profile_restrict': None})

import sqlalchemy.test.noseplugin
plugin = sqlalchemy.test.noseplugin.NoseSQLAlchemy()
import optparse
plugin.options(optparse.OptionParser())
plugin.configure(options, None)

# XXX: monkey-patch this so we can support it
import sqlalchemy.engine.url
def get_dialect(self):
    """Return the SQLAlchemy database dialect class corresponding to this URL's driver name."""
    
    try:
        module = getattr(__import__('sqlalchemy.databases.%s' % self.drivername).databases, self.drivername)
        return module.dialect
    except ImportError:
        # if sys.exc_info()[2].tb_next is None:
        if True:
            import pkg_resources
            for res in pkg_resources.iter_entry_points('sqlalchemy.databases'):
                if res.name == self.drivername:
                    return res.load()
        raise
sqlalchemy.engine.url.URL.get_dialect = get_dialect

import glob
test_files = glob.glob(TEST_DIR + "/test*.py") + glob.glob(TEST_DIR + "/*/test*.py")
test_files.sort()

# These are the ones that pass on CPython (ie that we've stubbed enough of their testing
# infrastructure to run):
CPYTHON_PASSING = [
    'test.base.test_dependency',
    'test.base.test_except',
    'test.base.test_utils',
    'test.dialect.test_access',
    'test.dialect.test_informix',
    'test.dialect.test_sybase',
    'test.engine.test_bind',
    'test.engine.test_ddlevents',
    'test.engine.test_execute',
    'test.engine.test_metadata',
    'test.engine.test_parseconnect',
    'test.engine.test_pool',
    'test.engine.test_reconnect',
    'test.ex.test_examples',
    'test.ext.test_associationproxy',
    'test.ext.test_compiler',
    'test.ext.test_orderinglist',
    'test.orm.test_association',
    'test.orm.test_assorted_eager',
    'test.orm.test_attributes',
    'test.orm.test_backref_mutations',
    'test.orm.test_bind',
    'test.orm.test_collection',
    'test.orm.test_compile',
    'test.orm.test_deprecations',
    'test.orm.test_dynamic',
    'test.orm.test_extendedattr',
    'test.orm.test_instrumentation',
    'test.sql.test_columns',
    'test.sql.test_constraints',
    'test.sql.test_labels',
    'test.sql.test_quote',
    'test.sql.test_rowcount',
    'test.sql.test_select',
    'test.sql.test_selectable',
    'test.zblog.test_zblog',
]

MODULES_TO_TEST = [
]

FAILING = [
]

class SkipTest(Exception):
    pass

# XXX we don't support this yet
import sqlalchemy.test.testing
def resolve_artifact_names(fn):
    raise SkipTest()
sqlalchemy.test.testing.resolve_artifact_names = resolve_artifact_names

_gc_collect = gc.collect
def gc_collect():
    # Not quite safe it seems to throw a SkipTest, since for some tests that will leave
    # things in an inconsistent state
    raise SystemError("Need to skip this test.")
gc.collect = gc_collect

MODULES_TO_TEST = CPYTHON_PASSING
MODULES_TO_TEST.remove('test.orm.test_extendedattr') # wants to set an int in the class locals
# MODULES_TO_TEST = ['test.ext.test_associationproxy']
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

    if mname == 'test.sql.test_select' and run_idx > 0:
        continue

    '''
    # These tests are slow:
    if 'test_pool' in fn:
        continue
    if 'test_reconnect' in fn:
        continue
    if 'test_associationproxy' in fn:
        continue
    '''

    print '=' * 50
    print mname

    try:
        try:
            m = __import__(mname, fromlist=["__all__"])
        except SkipTest:
            print "(skipping)"
            continue
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

                # These tests should be marked as requiring predictable_pc
                if (clsname == "SubclassGrowthTest" and t == "test_subclass"):
                    continue
                if (clsname == "PoolTest" and t == "test_listeners"):
                    continue
                if (clsname == "QueuePoolTest" and t == "test_mixed_close"):
                    continue
                if (clsname == "MockReconnectTest" and (t == "test_reconnect" or t == 'test_conn_reusable' or
                        t == 'test_invalidate_trans')):
                    continue
                if 'weak' in t or t.endswith('_gc'):
                    continue
                if (clsname == 'ReconstitutionTest' and t == 'test_copy'):
                    continue

                # This test is flaky since it depends on set ordering.
                # (It causes sporadic failures in cpython as well.)
                if clsname == "SelectTest" and t == 'test_binds':
                    continue

                # This test relies on quick destruction of cursor objects,
                # since it's not getting freed explicitly.
                if 'test_bind' in mname and t == 'test_clauseelement':
                    continue

                print "Running", t
                try:
                    try:
                        n.setup()
                    except AttributeError:
                        pass
                    getattr(n, t)()
                    try:
                        n.teardown()
                    except AttributeError:
                        pass
                except SkipTest:
                    pass
            if hasattr(cls, "teardown_class"):
                cls.teardown_class()
            _gc_collect()
    except Exception:
        # raise
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
