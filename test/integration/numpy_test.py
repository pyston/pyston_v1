# skip-if: True
# script expects to find the numpy directory at the same level as the Pyston directory
import os
import sys
import subprocess
import shutil

ENV_NAME = "numpy_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)
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
CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "Cython-0.22"))
NUMPY_DIR = ENV_NAME + "/../../numpy"

print "\n>>>"
print ">>> Setting up Cython..."
if not os.path.exists(CYTHON_DIR):
    print ">>>"

    url = "http://cython.org/release/Cython-0.22.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Cython-0.22.tar.gz"], cwd=SRC_DIR)

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "Cython_0001-Pyston-change-we-don-t-support-custom-traceback-entr.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
    print "Applied Cython patch"


    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
    subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
else:
    print ">>> Cython already installed."
    print ">>>"

print "\n>>>"
print ">>> Setting up NumPy..."
print ">>>"
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NUMPY_DIR)
subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=NUMPY_DIR)

print
print "PASSED"
