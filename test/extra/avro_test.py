import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "avro_test_env_" + os.path.basename(sys.executable)
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
NOSETESTS_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))
AVRO_DIR = os.path.abspath(os.path.join(ENV_NAME, "avro-1.7.7"))

packages = ["nose==1.3.7", "avro==1.7.7"] 
create_virtenv(ENV_NAME, packages, force_create = True)

url = "https://pypi.python.org/packages/source/a/avro/avro-1.7.7.tar.gz"
subprocess.check_call(["wget", url], cwd=ENV_NAME)
subprocess.check_call(["tar", "-zxf", "avro-1.7.7.tar.gz"], cwd=ENV_NAME)

env = os.environ
env["PYTHONPATH"] = os.path.abspath(os.path.join(ENV_NAME, "site-packages"))

# this tests also fail when run in cpython with nose.
# pytest makes two of this work but we can't currently run pytest...
expected = [{'ran': 51, 'errors': 3}]
run_test([NOSETESTS_EXE], env=env, cwd=AVRO_DIR, expected=expected)
