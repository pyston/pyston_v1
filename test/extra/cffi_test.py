# Note: the expected counts here are set to match the CI, and I can't reproduce them locally
import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "cffi_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
PYTEST_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "py.test"))

def install_and_test_cffi():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "https://pypi.python.org/packages/source/c/cffi/cffi-1.2.1.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "cffi-1.2.1.tar.gz"], cwd=SRC_DIR)
    CFFI_DIR = os.path.abspath(os.path.join(SRC_DIR, "cffi-1.2.1"))

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "cffi-1.2.1.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=CFFI_DIR)

    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=CFFI_DIR)

    # looks like clang 3.5 causes more errors like: 214 != -42 doing casts
    if os.environ.has_key("CC") and "clang" in os.environ["CC"]:
        expected = [{ "failed": 20, "passed": 1659, "skipped": 73, "xfailed": 4}]
        expected_log_hash = '''
        oRkAgDIgEgAAwoKiAIQAIABAQAAAAAIKBOAIUABAEAAAIMQFgQCKhKEgERFEMAgAAAIBAAiCCBAC
        CAIASESQBAQDpAEAAAogAAMBAoVQqkCKABBAAIDgAKECABJAAQiEIAAgAgOigAIwcoQBIAAACoAG
        2FIHAAQAJIELIVABgwA=
        '''
    else:
        expected = [{ "failed": 11, "passed": 1668, "skipped": 73, "xfailed": 4}]
        expected_log_hash = '''
        oRkAwBAg0gAEwoCiQIQgIQBAQAABQEKKBGAZVAhKcAAAAMQFAQAogKggFRFGEIgAAAKABgiGCBCC
        CAIASEAQHAQSpAEADEugCJEBAoFgIECDBBBEAACgACECAAJKgQicIAAgAAOChBIyUoQBIAAACoAG
        2FInAAQQpIEHARAJowE=
        '''
    run_test([PYTEST_EXE], cwd=CFFI_DIR, expected=expected, expected_log_hash=expected_log_hash)

create_virtenv(ENV_NAME, ["pytest==2.8.7", "py==1.4.31", "pycparser==2.14"], force_create = True)
install_and_test_cffi()
