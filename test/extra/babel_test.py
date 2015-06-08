# skip-if: True
import os, sys, subprocess
from test_helper import create_virtenv, run_test_and_parse_output

ENV_NAME = "babel_test_env_" + os.path.basename(sys.executable)
packages = ["nose", "-e", "git+http://github.com/mitsuhiko/babel.git@1.3#egg=Babel"]
create_virtenv(ENV_NAME, packages, force_create = True)

BABEL_DIR = os.path.abspath(os.path.join(ENV_NAME, "src", "babel"))            
python_exe = os.path.abspath(ENV_NAME + "/bin/python")
nosetests_exe = os.path.abspath(ENV_NAME + "/bin/nosetests")

subprocess.check_call([python_exe, "setup.py", "import_cldr"], cwd=BABEL_DIR)
subprocess.check_call([python_exe, "setup.py", "build"], cwd=BABEL_DIR)
subprocess.check_call([python_exe, "setup.py", "install"], cwd=BABEL_DIR)

errcode, result, output = run_test_and_parse_output([nosetests_exe], cwd=BABEL_DIR)
print
print "Return code:", errcode
expected = [{"failures": 3, "errors": 3}]
if expected == result:
    print "Received expected output"
else:
    print >> sys.stderr, output
    print >> sys.stderr, "WRONG output"
    print >> sys.stderr, "is:", result
    print >> sys.stderr, "expected:", expected
    assert result == expected
