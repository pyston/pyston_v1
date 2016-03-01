import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "routes_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

pkg = ["nose==1.3.7", "paste==2.0.2", "six==1.10.0"]
pkg += ["-e", "git+https://github.com/bbangert/routes.git@v1.7.3#egg=Routes"]
create_virtenv(ENV_NAME, pkg, force_create = True)

ROUTES_DIR = os.path.abspath(os.path.join(SRC_DIR, "routes"))
expected = [{ "ran" : 141 }]
run_test([PYTHON_EXE, "setup.py", "test"], cwd=ROUTES_DIR, expected=expected)
