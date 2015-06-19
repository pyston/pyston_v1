import os, sys
from test_helper import create_virtenv, run_test

ENV_NAME = os.path.abspath("cheetah_test_env_" + os.path.basename(sys.executable))
create_virtenv(ENV_NAME, ["cheetah==2.4.4"], force_create = True)

cheetah_exe = os.path.join(ENV_NAME, "bin", "cheetah")
env = os.environ
env["PATH"] = env["PATH"] + ":" + os.path.join(ENV_NAME, "bin")
expected = [{'errors': 4, 'failures': 53}, {'errors': 232, 'failures': 53}]
run_test([cheetah_exe, "test"], cwd=ENV_NAME, expected=expected, env=env)
