import os, sys, subprocess, shutil
from test_helper import create_virtenv, run_test

ENV_NAME = "protobuf_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_protobuf():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "http://archive.ubuntu.com/ubuntu/pool/main/p/protobuf/protobuf_2.5.0.orig.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "protobuf_2.5.0.orig.tar.gz"], cwd=SRC_DIR)
    PROTOBUF_DIR = os.path.abspath(os.path.join(SRC_DIR, "protobuf-2.5.0"))

    INSTALL_DIR = os.path.join(SRC_DIR, "protobuf_install")
    subprocess.check_call(["./configure", "--prefix="+INSTALL_DIR], cwd=PROTOBUF_DIR)
    subprocess.check_call(["make", "-j4"], cwd=PROTOBUF_DIR)
    subprocess.check_call(["make", "install"], cwd=PROTOBUF_DIR)

    PROTOBUF_PY_DIR = os.path.join(PROTOBUF_DIR, "python")
    env = os.environ
    env["PATH"] = env["PATH"] + ":" + os.path.join(INSTALL_DIR, "bin")
    env["LD_LIBRARY_PATH"] = os.path.join(INSTALL_DIR, "lib")
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=PROTOBUF_PY_DIR, env=env)

    expected = [{"ran": 216}]
    run_test([PYTHON_EXE, "setup.py", "test"], cwd=PROTOBUF_PY_DIR, expected=expected, env=env)

create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_protobuf()
