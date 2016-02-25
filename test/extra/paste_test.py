# expected: fail

# The paste test suite uses pytest, so this test file incentally also tests pytest.
# Pytest relies quite heavily on a number of code introspection features, which
# we currently don't have. This includes:
# 1) The ast module
# 2) traceback.tb_frame, traceback.tb_next
import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "paste_test_env_" + os.path.basename(sys.executable)
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
PYTEST_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "py.test"))
PASTE_DIR = os.path.abspath(os.path.join(ENV_NAME, "paste"))
PASTE_TEST_DIR = os.path.abspath(os.path.join(PASTE_DIR, "tests"))

packages = ["pytest", "paste", "nose==1.3.7"]
create_virtenv(ENV_NAME, packages, force_create = True)

# Need the test files in the source
subprocess.check_call(["hg", "clone", "https://bitbucket.org/ianb/paste"], cwd=ENV_NAME)

print ">> "
print ">> Setting up paste..."
print ">> "
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=PASTE_DIR)

print ">> "
print ">> Running paste tests..."
print ">> "
subprocess.check_call([PYTEST_EXE], cwd=PASTE_TEST_DIR)
