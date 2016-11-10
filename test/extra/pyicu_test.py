import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "pyicu_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

def install_and_test_pyicu():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "http://download.icu-project.org/files/icu4c/4.2.1/icu4c-4_2_1-src.tgz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "icu4c-4_2_1-src.tgz"], cwd=SRC_DIR)
    ICU_DIR = os.path.abspath(os.path.join(SRC_DIR, "icu", "source"))

    INSTALL_DIR = os.path.join(SRC_DIR, "icu_install")
    subprocess.check_call(["./runConfigureICU", "Linux", "--prefix=" + INSTALL_DIR], cwd=ICU_DIR)
    subprocess.check_call(["make", "-j4"], cwd=ICU_DIR)
    subprocess.check_call(["make", "install"], cwd=ICU_DIR)

    url = "https://pypi.python.org/packages/source/P/PyICU/PyICU-1.0.1.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "PyICU-1.0.1.tar.gz"], cwd=SRC_DIR)
    PYICU_DIR = os.path.abspath(os.path.join(SRC_DIR, "PyICU-1.0.1"))

    PATCH_FILE = os.path.abspath(os.path.join(os.path.dirname(__file__), "pyicu_101.patch"))
    subprocess.check_call(["patch", "-p1", "--input=" + PATCH_FILE], cwd=PYICU_DIR)
    print "Applied PyICU patch"

    env = os.environ
    INC_DIR = os.path.abspath(os.path.join(INSTALL_DIR, "include"))
    LIB_DIR = os.path.abspath(os.path.join(INSTALL_DIR, "lib"))
    env["CFLAGS"] = env["CXXFLAGS"] = " ".join(["-I" + INC_DIR, "-L" + LIB_DIR])
    env["LD_LIBRARY_PATH"] = LIB_DIR
    subprocess.check_call([PYTHON_EXE, "setup.py", "build"], cwd=PYICU_DIR, env=env)
    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=PYICU_DIR, env=env)

    expected = [{'ran': 17}]
    expected_log_hash = '''
    gAAAAQAAABQAACBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARAAAAAAAAAAAAAAAIAIAAgAAAAAAA
    AAAAAAgAAAACAAAAAAAAAAIAAAiAAAgAAQQAAAAAABAAAIEBAAAAAAAAACAAAAAAAAAAIIAIAAAA
    AAAAAAAAAAAAAAACgAA=
    '''
    run_test([PYTHON_EXE, "setup.py", "test"], cwd=PYICU_DIR, expected=expected, expected_log_hash=expected_log_hash)

create_virtenv(ENV_NAME, None, force_create = True)
install_and_test_pyicu()
