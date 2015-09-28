import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "M2Crypto_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_lxml():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "https://pypi.python.org/packages/source/M/M2Crypto/M2Crypto-0.21.1.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "M2Crypto-0.21.1.tar.gz"], cwd=SRC_DIR)
    M2CRYPTO_DIR = os.path.abspath(os.path.join(SRC_DIR, "M2Crypto-0.21.1"))

    url = "http://archive.ubuntu.com/ubuntu/pool/main/m/m2crypto/m2crypto_0.21.1-3ubuntu5.debian.tar.gz"
    subprocess.check_call(["wget", url], cwd=M2CRYPTO_DIR)
    subprocess.check_call(["tar", "-zxf", "m2crypto_0.21.1-3ubuntu5.debian.tar.gz"], cwd=M2CRYPTO_DIR)

    debian_patches = ("0001-Import-inspect-in-urllib-2.patch",
                      "0002-Disable-SSLv2_method-when-disabled-in-OpenSSL-iself-.patch",
                      "0003-Look-for-OpenSSL-headers-in-usr-include-DEB_HOST_MUL.patch",
                      "skip_sslv2_tests.patch",
                      "fix_testsuite_ftbfs.patch",
                      "fix_testsuite_tls1.2.patch",
                      "fix_testsuite_sha256.patch")	

    for patch in debian_patches:
        PATCH_FILE = os.path.abspath(os.path.join(M2CRYPTO_DIR, "debian", "patches", patch))
        subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=M2CRYPTO_DIR)

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "M2Crypto_patch.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=M2CRYPTO_DIR)

    env = os.environ
    # M2Crypto can't find the opensslconf without this
    env["DEB_HOST_MULTIARCH"] = "/usr/include/x86_64-linux-gnu"
    # SWIG does not work with pyston if this define is not set
    env["CFLAGS"] = "-DSWIG_PYTHON_SLOW_GETSET_THIS" 
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=M2CRYPTO_DIR, env=env)

    expected = [{'ran': 235, 'failures': 1, 'errors': 7, 'skipped': 2}]
    run_test([PYTHON_EXE, "setup.py", "test"], cwd=M2CRYPTO_DIR, expected=expected)
   
create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_lxml()
