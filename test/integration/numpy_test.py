# script expects to find the numpy directory at the same level as the Pyston directory
import os
import sys
import subprocess
import shutil

ENV_NAME = "numpy_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

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
NUMPY_DIR = os.path.dirname(__file__) + "/../lib/numpy"

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


    try:
        subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
        subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)
    except:
        subprocess.check_call(["rm", "-rf", CYTHON_DIR])
else:
    print ">>> Cython already installed."
    print ">>>"

print "\n>>>"
print ">>> Patching NumPy..."
print ">>>"
NUMPY_PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "numpy_patch.patch"))
try:
    cmd = ["patch", "-p1", "--forward", "-i", NUMPY_PATCH_FILE, "-d", NUMPY_DIR]
    subprocess.check_output(cmd, stderr=subprocess.STDOUT)
except subprocess.CalledProcessError as e:
    print e.output
    if "Reversed (or previously applied) patch detected!  Skipping patch" not in e.output:
        raise e

print "\n>>>"
print ">>> Setting up NumPy..."
print ">>>"
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NUMPY_DIR)

print "\n>>>"
print ">>> Installing NumPy..."
print ">>>"
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

print "\n>>>"
print ">>> Running NumPy linear algebra test..."
print ">>>"
subprocess.check_call([PYTHON_EXE, "-c", script], cwd=CYTHON_DIR)

print "\n>>>"
print ">>> Running NumPy mandelbrot test..."
print ">>>"
subprocess.check_call([PYTHON_EXE, "-c", mandelbrot], cwd=CYTHON_DIR)

print "\n>>>"
print ">>> Unpatching NumPy..."
print ">>>"
cmd = ["patch", "-p1", "--forward", "-i", NUMPY_PATCH_FILE, "-R", "-d", NUMPY_DIR]
subprocess.check_output(cmd, stderr=subprocess.STDOUT)

print
print "PASSED"
