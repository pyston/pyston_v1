import os
import sys
import subprocess

sys.path.append(os.path.dirname(__file__) + "/../lib")
import test_helper

def print_progress_header(text):
    print "\n>>>"
    print ">>> " + text + "..."
    print ">>>"

ENV_NAME = "numpy_fulltest_env_" + os.path.basename(sys.executable)
DEPENDENCIES = ["nose==1.3.7"]

import platform
USE_CUSTOM_PATCHES = (platform.python_implementation() == "Pyston")

test_helper.create_virtenv(ENV_NAME, DEPENDENCIES)


SRC_DIR = ENV_NAME
ENV_DIR = os.path.abspath(ENV_NAME)
PYTHON_EXE = os.path.abspath(ENV_NAME + "/bin/python")
CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "cython"))
NUMPY_DIR = os.path.abspath(os.path.join(SRC_DIR, "numpy"))

print_progress_header("Setting up Cython...")
if not os.path.exists(CYTHON_DIR):

    url = "https://github.com/cython/cython"
    subprocess.check_call(["git", "clone", "--depth", "1", "--branch", "0.24", url], cwd=SRC_DIR)

    if USE_CUSTOM_PATCHES:
        PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "../integration/Cython-0.24.patch"))
        subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
        print ">>> Applied Cython patch"


    try:
        subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
        subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
    except:
        subprocess.check_call(["rm", "-rf", CYTHON_DIR])
else:
    print ">>> Cython already installed."

print_progress_header("Cloning up NumPy...")
if not os.path.exists(NUMPY_DIR):
    url = "https://github.com/numpy/numpy"
    subprocess.check_call(["git", "clone", "--depth", "1", "--branch", "v1.11.0", url], cwd=SRC_DIR)
else:
    print ">>> NumPy already installed."

try:
    env = os.environ
    CYTHON_BIN_DIR = os.path.abspath(os.path.join(ENV_NAME + "/bin"))
    env["PATH"] = CYTHON_BIN_DIR + ":" + env["PATH"]

    # Should be able to do this:
    # subprocess.check_call([os.path.join(SRC_DIR, "bin/pip"), "install", NUMPY_DIR])
    # but they end up naming f2py "f2py_release"/"f2py_dbg"

    print_progress_header("Setting up NumPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NUMPY_DIR, env=env)

    print_progress_header("Installing NumPy...")
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=NUMPY_DIR, env=env)
except:
    # TODO: I'm not sure we need to do this:
    subprocess.check_call(["rm", "-rf", NUMPY_DIR + "/build"])
    subprocess.check_call(["rm", "-rf", NUMPY_DIR + "/dist"])

    raise

test_helper.run_test(['sh', '-c', '. %s/bin/activate && python %s/numpy/tools/test-installed-numpy.py' % (ENV_DIR, ENV_DIR)],
        ENV_NAME, [dict(ran=6139, failures=1)])

print
print "PASSED"
