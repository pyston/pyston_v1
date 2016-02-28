import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "unidecode_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_unidecode():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)
    
    url = "https://pypi.python.org/packages/source/U/Unidecode/Unidecode-0.04.16.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Unidecode-0.04.16.tar.gz"], cwd=SRC_DIR)
    UNIDECODE_DIR = os.path.abspath(os.path.join(SRC_DIR, "Unidecode-0.04.16"))

    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=UNIDECODE_DIR)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=UNIDECODE_DIR)
    
    expected = [{'ran': 8}]
    run_test([PYTHON_EXE, "setup.py", "test"], cwd=UNIDECODE_DIR, expected=expected)
    
create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_unidecode()
