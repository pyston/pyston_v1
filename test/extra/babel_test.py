import os, sys, subprocess
from test_helper import create_virtenv, run_test

ENV_NAME = "babel_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
BABEL_DIR = os.path.abspath(os.path.join(ENV_NAME, "src", "babel"))
NOSETESTS_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))

packages = ["nose==1.3.7", "pytz==2015.4", "-e", "git+http://github.com/mitsuhiko/babel.git@1.3#egg=Babel"]
create_virtenv(ENV_NAME, packages, force_create = True)

subprocess.check_call([PYTHON_EXE, "setup.py", "import_cldr"], cwd=BABEL_DIR)
subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=BABEL_DIR)
subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=BABEL_DIR)

expected = [{"ran": 227, "failures": 3, "errors": 3}]
run_test([NOSETESTS_EXE], cwd=BABEL_DIR, expected=expected)
