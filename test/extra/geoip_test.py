import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "geoip_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

pkg = ["nose==1.3.7", "-e", "git+http://github.com/maxmind/geoip-api-python.git@v1.3.2#egg=GeoIP"]
create_virtenv(ENV_NAME, pkg, force_create = True)
GEOIP_DIR = os.path.abspath(os.path.join(SRC_DIR, "geoip"))
expected = [{'ran': 10}]

expected_log_hash = '''
ggAAAAAAQAQAAAAACAAAAAAAAAAAAAIABIAAAAAAgAACAAAAAAAIAAAAAAAAAAIIBAAAgABABAgA
AAAAAAAAAAAAAAAAAAAAAAQAAIgAAAAAAAAAAQBAABAAEAEAAAAAAAAAAAgAAAAIAIAAAAEAAAAA
AIAAAAgAAAAAAAAAAAA=
'''


run_test([PYTHON_EXE, "setup.py", "test"], cwd=GEOIP_DIR, expected=expected, expected_log_hash=expected_log_hash)
