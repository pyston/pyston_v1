import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "simplejson_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_simplejson():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "https://pypi.python.org/packages/source/s/simplejson/simplejson-2.6.2.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "simplejson-2.6.2.tar.gz"], cwd=SRC_DIR)
    SIMPLEJSON_DIR = os.path.abspath(os.path.join(SRC_DIR, "simplejson-2.6.2"))

    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=SIMPLEJSON_DIR)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=SIMPLEJSON_DIR)

    expected = [{'ran': 170}]
    expected_log_hash = '''
    gAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAAA
    AAAAAAAAAAAAAAAAAEAAAAQAAAgAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
    AAIAAAAAAAAAAAAAAAA=
    '''

    run_test([PYTHON_EXE, "setup.py", "test"], cwd=SIMPLEJSON_DIR, expected=expected, expected_log_hash=expected_log_hash)

create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_simplejson()
