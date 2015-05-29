import os, sys
from test_helper import create_virtenv, run_test_and_parse_output

ENV_NAME = "cheetah_test_env_" + os.path.basename(sys.executable)
create_virtenv(ENV_NAME, ["cheetah==2.4.4"], force_create = True)

cheetah_exe = os.path.abspath(ENV_NAME + "/bin/cheetah")
env = os.environ
env["PATH"] = env["PATH"] + ":" + os.path.abspath(ENV_NAME + "/bin")
errcode, result, output = run_test_and_parse_output([cheetah_exe, "test"], cwd=os.path.abspath(ENV_NAME), env= env)
print
print "Return code:", errcode
expected = [{'errors': 4, 'failures': 53}, {'errors': 232, 'failures': 53}]
if expected == result:
    print "Received expected output"
else:
    print >> sys.stderr, output
    print >> sys.stderr, "WRONG output"
    print >> sys.stderr, "is:", result
    print >> sys.stderr, "expected:", expected
    assert result == expected
