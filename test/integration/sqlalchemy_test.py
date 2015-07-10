# skip-if: True
# - getting to the point of running the sqlalchemy tests is quite hard, since there is a lot
#   of pytest code that runs beforehand.

import os
import sys
import subprocess
import shutil

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
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

# Exec'ing the test suite can be nice for debugging, but let's the subprocess version
# in case we want to do stuff afterwards:
do_exec = True

if do_exec:
    print "About to exec"
    os.chdir(SQLALCHEMY_DIR)
    os.execl(python_exe, python_exe, "-m", "pytest")
else:
    subprocess.check_call([python_exe, "-m", "pytest"], cwd=SQLALCHEMY_DIR)


print
print "PASSED"
