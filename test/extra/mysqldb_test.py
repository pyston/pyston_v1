import os, sys, subprocess, shutil
from test_helper import create_virtenv, run_test_and_parse_output

ENV_NAME = "mysqldb_test_env_" + os.path.basename(sys.executable)
packages = ["nose"]
create_virtenv(ENV_NAME, packages, force_create = True)

SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
shutil.rmtree(SRC_DIR, ignore_errors=True)
os.makedirs(SRC_DIR)
subprocess.check_call(["git", "clone", "https://github.com/farcepest/MySQLdb1.git"], cwd=SRC_DIR)

MYSQLDB_DIR = os.path.abspath(os.path.join(SRC_DIR, "MySQLdb1"))
subprocess.check_call(["git", "checkout", "MySQLdb-1.2.5"], cwd=MYSQLDB_DIR)      
python_exe = os.path.abspath(ENV_NAME + "/bin/python")
nosetests_exe = os.path.abspath(ENV_NAME + "/bin/nosetests")

#apply patch
PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "mysqldb_0001-Pyston-change-register-types.patch"))
subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=MYSQLDB_DIR)
print "Applied patch"

subprocess.check_call([python_exe, "setup.py", "build"], cwd=MYSQLDB_DIR)
subprocess.check_call([python_exe, "setup.py", "install"], cwd=MYSQLDB_DIR)

errcode, result, output = run_test_and_parse_output([nosetests_exe], cwd=MYSQLDB_DIR)
print
print "Return code:", errcode
expected = []
if expected == result:
    print "Received expected output"
else:
    print >> sys.stderr, output
    print >> sys.stderr, "WRONG output"
    print >> sys.stderr, "is:", result
    print >> sys.stderr, "expected:", expected
    assert result == expected

