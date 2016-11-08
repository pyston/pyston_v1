import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "pylons_test_env_" + os.path.basename(sys.executable)
SRC_DIR = os.path.abspath(os.path.join(ENV_NAME, "src"))
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))
NOSE_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))

def install_and_test_pylons():
    shutil.rmtree(SRC_DIR, ignore_errors=True)
    os.makedirs(SRC_DIR)

    url = "https://pypi.python.org/packages/source/P/Pylons/Pylons-0.9.6.2.tar.gz"
    subprocess.check_call(["wget", url], cwd=SRC_DIR)
    subprocess.check_call(["tar", "-zxf", "Pylons-0.9.6.2.tar.gz"], cwd=SRC_DIR)
    PYLONS_DIR = os.path.abspath(os.path.join(SRC_DIR, "Pylons-0.9.6.2"))

    subprocess.check_call([PYTHON_EXE, "setup.py", "install"], cwd=PYLONS_DIR)

    # most of the errors are because of our coerceUnicodeToStr which raises a TypeError instead of a UnicodeError
    # but as we don't support the unicode string correctly every where I don't want to change this currently.
    expected = [{ "ran": 50, "errors": 7}]
    expected_log_hash = '''
    wLKBAAEAEQAABEAgAAUAYBABtBACiIFIAoAIIAiAYAIUBADgCOIAggAIBACQCAgIgAGBgCAsAIAB
    FCIAQAAQAQQAmQoAAACEMQAiAaIAFIgAEEAAAUgAAGAIQAEAAEBQQABQAEAAAAAAAiEiIEAAAEIC
    ECBAiigwIAAABAQIAQE=
    '''
    run_test([NOSE_EXE], cwd=PYLONS_DIR, expected=expected, expected_log_hash=expected_log_hash)

pkg = [ "Mako==1.0.3",
        "decorator==4.0.9",
        "simplejson==3.8.2",
        "FormEncode==1.3.0",
        "PasteScript==2.0.2",
        "PasteDeploy==1.5.2",
        "Paste==2.0.2",
        "Beaker==1.8.0",
        "WebHelpers==1.3",
        "Routes==2.2",
        "MarkupSafe==0.23",
        "six==1.10.0",
        "funcsigs==0.4",
        "repoze.lru==0.6",
        "nose==1.3.7"]

create_virtenv(ENV_NAME, pkg, force_create = True)
install_and_test_pylons()
