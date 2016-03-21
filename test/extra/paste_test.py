import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "paste_test_env_" + os.path.basename(sys.executable)
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
PYTEST_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "py.test"))

packages = ["py==1.4.31", "pytest==2.8.7", "nose==1.3.7"]
create_virtenv(ENV_NAME, packages, force_create = True)

# Need the test files in the source
url = "https://pypi.python.org/packages/source/P/Paste/Paste-1.7.5.tar.gz"
subprocess.check_call(["wget", url], cwd=ENV_NAME)
subprocess.check_call(["tar", "-zxf", "Paste-1.7.5.tar.gz"], cwd=ENV_NAME)
PASTE_DIR = os.path.abspath(os.path.join(ENV_NAME, "Paste-1.7.5"))
PASTE_TEST_DIR = os.path.abspath(os.path.join(PASTE_DIR, "tests"))

print ">> "
print ">> Setting up paste..."
print ">> "
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=PASTE_DIR)

print ">> "
print ">> Installing paste..."
print ">> "
subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=PASTE_DIR)

print ">> "
print ">> Running paste tests..."
print ">> "
# current cpython also does not pass all tests. (9 failed, 127 passed)
# the additional test we fail are because:
# - we have str.__iter__
# - no sys.settrace 
# - no shiftjis encoding
# - slightly different error messages
expected = [{"failed" : 22, "passed" : 112}]
run_test([PYTEST_EXE], cwd=PASTE_TEST_DIR, expected=expected)



