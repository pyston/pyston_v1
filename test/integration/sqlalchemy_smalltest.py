# run_args: -x

import os
import sys
import subprocess
import traceback

ENV_NAME = "sqlalchemy_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/virtualenv/virtualenv.py"

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

SQLALCHEMY_DIR = os.path.dirname(__file__) + "/sqlalchemy"
TEST_DIR = SQLALCHEMY_DIR + "/test"
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

sys.path.append(SQLALCHEMY_DIR + "/lib")
sys.path.insert(0, SQLALCHEMY_DIR)
sys.path.append(ENV_NAME + "/site-packages")
sys.path.append(ENV_NAME + "/lib/python2.7/site-packages")

# make sure this is importable:
import mock

import sqlalchemy.testing
class Requirements(object):
    def __getattr__(self, n):
        def inner(f):
            return f
        inner.not_ = lambda: inner
        return inner
sqlalchemy.testing.config.requirements = sqlalchemy.testing.requires = Requirements()

import glob
test_files = glob.glob(TEST_DIR + "/test*.py") + glob.glob(TEST_DIR + "/*/test*.py")

# These are the ones that pass on CPython (ie that we've stubbed enough of their testing
# infrastructure to run):
MODULES_TO_TEST = ['test.engine.test_parseconnect', 'test.ext.test_compiler', 'test.dialect.test_pyodbc', 'test.dialect.test_sybase', 'test.dialect.test_mxodbc', 'test.sql.test_inspect', 'test.sql.test_operators', 'test.sql.test_ddlemit', 'test.sql.test_cte', 'test.base.test_dependency', 'test.base.test_except', 'test.base.test_inspect', 'test.base.test_events', 'test.orm.test_inspect', 'test.orm.test_descriptor']

# These are currently broken on Pyston:
MODULES_TO_TEST.remove("test.sql.test_operators")
MODULES_TO_TEST.remove("test.base.test_events")
MODULES_TO_TEST.remove("test.orm.test_descriptor")

passed = []
failed = []

for fn in test_files:
    assert fn.startswith(TEST_DIR + '/')
    assert fn.endswith(".py")
    mname = fn[len(SQLALCHEMY_DIR) + 1:-3].replace('/', '.')
    if mname not in MODULES_TO_TEST:
        continue
    print
    print mname

    try:
        m = __import__(mname, fromlist=["__all__"])
        for nname in dir(m):
            n = getattr(m, nname)
            if not nname.endswith("Test") or not isinstance(n, type):
                continue
            print "Running", n

            n = n()
            for t in dir(n):
                if not t.startswith("test_"):
                    continue
                print "Running", t
                n.setup()
                getattr(n, t)()
                n.teardown()
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
