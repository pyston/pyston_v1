# skip-if: 'clang' in os.environ['CC']
# looks like libsass only builds using gcc...
import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "sass_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
SASS_DIR = os.path.abspath(os.path.join(ENV_NAME, "src", "libsass"))

packages = ["six==1.10", "werkzeug==0.9"]
packages += ["-e", "git+https://github.com/dahlia/libsass-python@0.8.3#egg=libsass"]
create_virtenv(ENV_NAME, packages, force_create = True)

expected = [{'ran': 75}]
run_test([PYTHON_EXE, "setup.py", "test"], cwd=SASS_DIR, expected=expected)

