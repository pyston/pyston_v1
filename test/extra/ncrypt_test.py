import os, sys, subprocess, shutil
from test_helper import create_virtenv, run_test

ENV_NAME = "ncrypt_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_pyrex():
    url = "http://www.cosc.canterbury.ac.nz/greg.ewing/python/Pyrex/oldtar/Pyrex-0.9.8.6.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Pyrex-0.9.8.6.tar.gz"], cwd=SRC_DIR)
    PYREX_DIR = os.path.abspath(os.path.join(SRC_DIR, "Pyrex-0.9.8.6"))

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "Pyrex_0.9.8.6.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=PYREX_DIR)
    print "Applied pyrex patch"
    
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=PYREX_DIR)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=PYREX_DIR)

def install_and_test_ncrypt():
    url = "http://archive.ubuntu.com/ubuntu/pool/universe/n/ncrypt/ncrypt_0.6.4.orig.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "ncrypt_0.6.4.orig.tar.gz"], cwd=SRC_DIR)

    url = "http://archive.ubuntu.com/ubuntu/pool/universe/n/ncrypt/ncrypt_0.6.4-0ubuntu8.diff.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["gunzip", "ncrypt_0.6.4-0ubuntu8.diff.gz"], cwd=SRC_DIR)
    NCRYPT_DIR = os.path.abspath(os.path.join(SRC_DIR, "ncrypt-0.6.4"))

    PATCH_FILE = os.path.abspath(os.path.join(SRC_DIR, "ncrypt_0.6.4-0ubuntu8.diff"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=NCRYPT_DIR)
    print "Applied ncrypt patch"

    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=NCRYPT_DIR)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=NCRYPT_DIR)
    
    expected = [{"failures": 1, "errors": 0}]
    test_file = os.path.join(NCRYPT_DIR, "ncrypt", "test", "test.py")
    run_test([PYTHON_EXE, test_file], cwd=NCRYPT_DIR, expected=expected)
    
create_virtenv(ENV_NAME, None, force_create = True)
shutil.rmtree(SRC_DIR, ignore_errors=True)
os.makedirs(SRC_DIR)
install_pyrex()
install_and_test_ncrypt()
