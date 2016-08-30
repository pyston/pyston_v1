# skip-if: True
# This test will take a long time(51mins).
# 3100.61s user 490.14s system 118% cpu 50:30.63 total
# So please test it manually.

import os
import sys
import subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")
from test_helper import run_test

"""
Using this test file.

Note that sometimes it can be a pain to run this script everytime you make a
change, as it does take a while. You can cd to the scipy_test_env_* directory
and run the binaries inside of that folder, which will be able to "see" the
SciPy module. For example:

~/pyston/scipy_test_env_pyston_dbg$ gdb --args ./bin/python -c "import scipy; scipy.test()"

The /bin/python is the pyston executable so if you recompile pyston, you need to run this
script again to update it.

"""

def print_progress_header(text):
    print "\n>>>"
    print ">>> " + text + "..."
    print ">>>"

ENV_NAME = "scipy_test_env_" + os.path.basename(sys.executable)
DEPENDENCIES = ["nose==1.3.7"]

import platform
USE_CUSTOM_PATCHES = (platform.python_implementation() == "Pyston")

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)

        subprocess.check_call([ENV_NAME + "/bin/pip", "install"] + DEPENDENCIES)
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]

SRC_DIR = ENV_NAME
PYTHON_EXE = os.path.abspath(ENV_NAME + "/bin/python")
CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "cython"))
NUMPY_DIR = os.path.abspath(os.path.join(SRC_DIR, "numpy"))
SCIPY_DIR = os.path.abspath(os.path.join(SRC_DIR, "scipy"))
SKLEARN_DIR = os.path.abspath(os.path.join(SRC_DIR, "scikit-learn"))

print_progress_header("Setting up Cython...")
if not os.path.exists(CYTHON_DIR):

    url = "git@github.com:Daetalus/cython.git"
    subprocess.check_call(["git", "clone", "-b", "pyston_patch", url], cwd=SRC_DIR)

    # if USE_CUSTOM_PATCHES:
    #     PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "Cython-0.24.patch"))
    #     subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
    #     print ">>> Applied Cython patch"

    try:
        subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
        subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
    except:
        subprocess.check_call(["rm", "-rf", CYTHON_DIR])
else:
    print ">>> Cython already installed."

env = os.environ
CYTHON_BIN_DIR = os.path.abspath(os.path.join(ENV_NAME + "/bin"))
env['NPY_NUM_BUILD_JOBS'] = '8'
env["PATH"] = CYTHON_BIN_DIR + ":" + env["PATH"]

print_progress_header("Cloning up NumPy...")
if not os.path.exists(NUMPY_DIR):
    url = "https://github.com/numpy/numpy"
    subprocess.check_call(["git", "clone", "--depth", "1", "--branch", "v1.11.0", url], cwd=SRC_DIR)
else:
    print ">>> NumPy already installed."

try:
    print_progress_header("Setting up NumPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NUMPY_DIR, env=env)

    print_progress_header("Installing NumPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=NUMPY_DIR, env=env)
except:
    subprocess.check_call(["rm", "-rf", NUMPY_DIR + "/build"])
    subprocess.check_call(["rm", "-rf", NUMPY_DIR + "/dist"])

    raise

if not os.path.exists(SCIPY_DIR):
    print_progress_header("Cloning up SciPy...")
    url = "https://github.com/scipy/scipy"
    subprocess.check_call(["git", "clone", "--depth", "1", "--branch", "v0.17.1", url], cwd=SRC_DIR)
else:
    print ">>> SciPy already installed."

try:
    print_progress_header("Setting up SciPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=SCIPY_DIR, env=env)

    print_progress_header("Installing SciPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=SCIPY_DIR, env=env)
except:
    subprocess.check_call(["rm", "-rf", SCIPY_DIR + "/build"])
    subprocess.check_call(["rm", "-rf", SCIPY_DIR + "/dist"])
    raise

if not os.path.exists(SKLEARN_DIR):
    print_progress_header("Cloning up Scikit-learn...")
    url = "git@github.com:scikit-learn/scikit-learn.git"
    subprocess.check_call(["git", "clone", url], cwd=SRC_DIR)
else:
    print ">>> Scikit-learn already installed."

try:
    print_progress_header("Build scikit-learn inplace...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "build_ext", "--inplace"], cwd=SKLEARN_DIR, env=env)
except:
    subprocess.check_call(["rm", "-rf", SCIPY_DIR + "/build"])
    raise


print
print "PASSED"
