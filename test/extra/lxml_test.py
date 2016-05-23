import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "lxml_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_lxml():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "http://cython.org/release/Cython-0.22.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Cython-0.22.tar.gz"], cwd=SRC_DIR)

    CYTHON_DIR = os.path.abspath(os.path.join(SRC_DIR, "Cython-0.22"))
    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "integration", "Cython-0.22.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CYTHON_DIR)
    print "Applied Cython patch"
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CYTHON_DIR)
    subprocess.check_call([PYTHON_EXE, "-c", "import Cython"], cwd=CYTHON_DIR)

    url = "https://pypi.python.org/packages/source/l/lxml/lxml-3.0.1.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "lxml-3.0.1.tar.gz"], cwd=SRC_DIR)
    LXML_DIR = os.path.abspath(os.path.join(SRC_DIR, "lxml-3.0.1"))

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "lxml_patch.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=LXML_DIR)
    print "Applied lxml patch"

    subprocess.check_call([PYTHON_EXE, "setup.py", "build_ext", "-i", "--with-cython"], cwd=LXML_DIR)
 
    expected = [{'ran': 1381, 'failures': 3, 'errors': 1}]
    run_test([PYTHON_EXE, "test.py"], cwd=LXML_DIR, expected=expected)
    
create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_lxml()
