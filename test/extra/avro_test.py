import os, sys, subprocess
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "avro_test_env_" + os.path.basename(sys.executable)
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
PYTEST_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "py.test"))
AVRO_DIR = os.path.abspath(os.path.join(ENV_NAME, "avro-1.7.7"))

packages = ["pytest==2.8.7", "py==1.4.29", "avro==1.7.7"]
create_virtenv(ENV_NAME, packages, force_create = True)

url = "https://pypi.python.org/packages/source/a/avro/avro-1.7.7.tar.gz"
subprocess.check_call(["wget", url], cwd=ENV_NAME)
subprocess.check_call(["tar", "-zxf", "avro-1.7.7.tar.gz"], cwd=ENV_NAME)

env = os.environ
env["PYTHONPATH"] = os.path.abspath(os.path.join(ENV_NAME, "lib/python2.7/site-packages"))

# cpython has the same number of failures
expected = [{'failed': 1, 'passed': 48}]
expected_log_hash = '''
gBAAAgCAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAgBAAAAAAAAIAMBAQAACAAABAAQAACAAAAA
AAEAAAAAAABAAAAAAIAAAAAAAAAAAAgACAAAEAAAAAAAAAAAAACAAAAAAAEAAAAAAAAAAAAAAAAA
IAQAAAAAAAAAAAAAAAA=
'''
run_test([PYTEST_EXE], env=env, cwd=AVRO_DIR, expected=expected, expected_log_hash=expected_log_hash)
