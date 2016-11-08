import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "babel_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
BABEL_DIR = os.path.abspath(os.path.join(ENV_NAME, "src", "babel"))
NOSETESTS_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))

packages = ["nose==1.3.7", "pytz==2015.4", "-e", "git+http://github.com/mitsuhiko/babel.git@1.3#egg=Babel"]
create_virtenv(ENV_NAME, packages, force_create = True)

PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "babel.patch"))
subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=BABEL_DIR)

subprocess.check_call([PYTHON_EXE, "setup.py", "import_cldr"], cwd=BABEL_DIR)
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=BABEL_DIR)
subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=BABEL_DIR)

expected = [{"ran": 227, "failures": 3, "errors": 3}]
expected_log_hash = '''
gAIAAAAACQAAAABAAAAABAAAAIAAEAAAAAAAAAAAAEAEBAAAAAAAkAAAAAAAAAAAQAAEgAAAAAAA
AAAAAAAAAQAACAgAAAAAAAAAIAAJAAAAAAAAAAAAAAAAAAAAAEAAAAAAAhAAAAAAAAAAEACAAAAA
EIgAAAAQAAAAAIAAAAA=
'''
run_test([NOSETESTS_EXE], cwd=BABEL_DIR, expected=expected, expected_log_hash=expected_log_hash)
