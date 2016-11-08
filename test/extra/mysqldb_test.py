# This tests needs mysql and a database:
# mysql -e 'create database mysqldb_test charset utf8;'

import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "mysqldb_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_mysqldb():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)
    subprocess.check_call(["git", "clone", "https://github.com/farcepest/MySQLdb1.git"], cwd=SRC_DIR)

    MYSQLDB_DIR = os.path.abspath(os.path.join(SRC_DIR, "MySQLdb1"))
    subprocess.check_call(["git", "checkout", "MySQLdb-1.2.5"], cwd=MYSQLDB_DIR)

    nosetests_exe = os.path.abspath(ENV_NAME + "/bin/nosetests")

    #apply patch
    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "mysqldb_0001-Pyston-change-register-types.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=MYSQLDB_DIR)
    print "Applied mysqldb patch"

    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=MYSQLDB_DIR)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=MYSQLDB_DIR)

    env = os.environ
    env["TESTDB"] = "travis.cnf"
    expected = [{"ran": 69}]
    expected_log_hash = '''
    gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAAA
    AAAAAAAEAAAAAAAAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
    AAAAAAAAAAAQAAAAAAA=
    '''
    run_test([nosetests_exe], cwd=MYSQLDB_DIR, expected=expected, env=env, expected_log_hash=expected_log_hash)

packages = ["nose==1.3.7"]
create_virtenv(ENV_NAME, packages, force_create = True)
install_and_test_mysqldb()
