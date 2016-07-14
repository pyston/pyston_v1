import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "formencode_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
NOSETESTS_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))
FORMENCODE_DIR = os.path.abspath(os.path.join(ENV_NAME, "src", "formencode"))

packages = ["nose==1.3.7", "pycountry==1.6", "pyDNS==2.3.6"]
packages += ["-e", "git+https://github.com/formencode/formencode.git@1.2.5#egg=formencode"]
create_virtenv(ENV_NAME, packages, force_create = True)

subprocess.check_call(["patch", "-p1"], stdin=open(os.path.join(os.path.dirname(__file__), "formencode.patch")), cwd=SRC_DIR)

expected = [{'ran': 201}]
run_test([NOSETESTS_EXE], cwd=FORMENCODE_DIR, expected=expected)
