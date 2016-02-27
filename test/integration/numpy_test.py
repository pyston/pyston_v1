import os
import sys
import subprocess
import shutil

"""
Using this test file.

We apply some patches on NumPy for some issues that we can't fix at the moment. The
patches are applied directly to the NumPy subrepository (test/lib/numpy). If you need,
you can make modifications in that folder directly for testing purposes. Just make sure
that everytime you do, run this script again, so that the contents of numpy_test_env_*
are updated (this is where the code and binaries that get tested are located).

Note that sometimes it can be a pain to run this script everytime you make a change,
as it does take a while. You can cd to the numpy_test_env_* directory and run the
binaries inside of that folder, which will be able to "see" the NumPy module. For example:

~/pyston/numpy_test_env_pyston_dbg$ gdb --args ./bin/python -c "import numpy as np; np.test()"

The /bin/python is the pyston executable so if you recompile pyston, you need to run this
script again to update it.

Currently this script is not running the NumPy tests since there are still crashes
happening. If you want to run the test, go to the bottom of the file and uncomment
the subprocess call to the test suite.

Some test cases in test/lib/numpy are commented out by the patch since they caused
a crash where the cause was not immediately obvious and I wanted to make progress. Those
will need to be uncommented at some point.
"""

def print_progress_header(text):
    print "\n>>>"
    print ">>> " + text + "..."
    print ">>>"

ENV_NAME = "numpy_test_env_" + os.path.basename(sys.executable)
DEPENDENCIES = ["nose==1.3.7"]

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
CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "Cython-0.22"))
NUMPY_DIR = os.path.dirname(__file__) + "/../lib/numpy"

print_progress_header("Setting up Cython...")
if not os.path.exists(CYTHON_DIR):

    url = "http://cython.org/release/Cython-0.22.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Cython-0.22.tar.gz"], cwd=SRC_DIR)

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "Cython-0.22.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
    print ">>> Applied Cython patch"


    try:
        subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
        subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
    except:
        subprocess.check_call(["rm", "-rf", CYTHON_DIR])
else:
    print ">>> Cython already installed."

print_progress_header("Patching NumPy...")
NUMPY_PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "numpy_patch.patch"))
try:
    cmd = ["patch", "-p1", "--forward", "-i", NUMPY_PATCH_FILE, "-d", NUMPY_DIR]
    subprocess.check_output(cmd, stderr=subprocess.STDOUT)
except subprocess.CalledProcessError as e:
    print e.output
    if "Reversed (or previously applied) patch detected!  Skipping patch" not in e.output:
        raise e

print_progress_header("Setting up NumPy...")
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NUMPY_DIR)

print_progress_header("Installing NumPy...")
subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=NUMPY_DIR)

# From Wikipedia
script = """
import numpy as np
from numpy.linalg import solve, inv
a = np.array([[1, 2, 3], [3, 4, 6.7], [5, 9.0, 5]])
b = np.array([3, 2, 1])
a.transpose()
inv(a)
solve(a, b)
print a, b
"""

# http://wiki.scipy.org/Tentative_NumPy_Tutorial/Mandelbrot_Set_Example
mandelbrot = """
from numpy import *

def mandelbrot( h,w, maxit=20 ):
        '''Returns an image of the Mandelbrot fractal of size (h,w).
        '''
        y,x = ogrid[ -1.4:1.4:h*1j, -2:0.8:w*1j ]
        c = x+y*1j
        z = c
        divtime = maxit + zeros(z.shape, dtype=int)

        for i in xrange(maxit):
                z  = z**2 + c
                diverge = z*conj(z) > 2**2
                div_now = diverge & (divtime==maxit)
                divtime[div_now] = i
                z[diverge] = 2

        return divtime

m = mandelbrot(complex(400),complex(400))
print m
"""

# this is just a small test which checks if numpy is able to manually create a pyston string
string_test = """
import numpy as np
a = np.array([1, 2**16, 2**32], dtype=int)
s = a.astype(str)
print s
assert repr(s) == "array(['1', '65536', '4294967296'], \\n      dtype='|S21')"
"""

numpy_test = "import numpy as np; np.test(verbose=2)"

print_progress_header("Running NumPy linear algebra test...")
subprocess.check_call([PYTHON_EXE, "-c", script], cwd=CYTHON_DIR)

print_progress_header("Running NumPy mandelbrot test...")
subprocess.check_call([PYTHON_EXE, "-c", mandelbrot], cwd=CYTHON_DIR)


print_progress_header("Running NumPy str test...")
subprocess.check_call([PYTHON_EXE, "-c", string_test], cwd=CYTHON_DIR)

# fails in release mode
#print_progress_header("Running NumPy test_abc.py test...")
#subprocess.check_call([PYTHON_EXE, os.path.join(NUMPY_DIR, "numpy/core/tests/test_abc.py")], cwd=CYTHON_DIR)

print_progress_header("Running NumPy test suite...")
# Currently we crash running the test suite. Uncomment for testing or
# when all the crashes are fixed.
# subprocess.check_call([PYTHON_EXE, "-c", numpy_test], cwd=CYTHON_DIR)

print_progress_header("Unpatching NumPy...")
cmd = ["patch", "-p1", "--forward", "-i", NUMPY_PATCH_FILE, "-R", "-d", NUMPY_DIR]
subprocess.check_output(cmd, stderr=subprocess.STDOUT)

print
print "PASSED"
