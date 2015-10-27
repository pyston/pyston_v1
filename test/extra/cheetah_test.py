import os, sys
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = os.path.abspath("cheetah_test_env_" + os.path.basename(sys.executable))
create_virtenv(ENV_NAME, ["cheetah==2.4.4", "Markdown==2.0.1"], force_create = True)

cheetah_exe = os.path.join(ENV_NAME, "bin", "cheetah")
env = os.environ
env["PATH"] = os.path.join(ENV_NAME, "bin")
expected = [{'ran': 2138, 'errors': 4, 'failures': 1}, {'ran': 2138, 'errors': 232, 'failures': 3}]
run_test([cheetah_exe, "test"], cwd=ENV_NAME, expected=expected, env=env)
