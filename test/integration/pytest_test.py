import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "pytest_test_env_" + os.path.basename(sys.executable)
ENV_DIR = os.path.abspath(ENV_NAME)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

pkg = ["pytest==2.8.2"]
create_virtenv(ENV_NAME, pkg)
PYTEST_DIR = os.path.abspath(os.path.join(SRC_DIR, "pytest"))

test_dir = os.path.join(ENV_DIR, "tests")
if not os.path.exists(test_dir):
    os.mkdir(test_dir)

with open(os.path.join(test_dir, "test_foo.py"), 'w') as f:
    f.write("""
import pytest
@pytest.mark.skipif(True, reason="for fun")
def test_skipif_true():
    1/0
""")

subprocess.check_call([os.path.join(ENV_DIR, "bin", "py.test"), test_dir])
# subprocess.check_call(["gdb", "--args", PYTHON_EXE, "-m", "pytest", test_dir])


