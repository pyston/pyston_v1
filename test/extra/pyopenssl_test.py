import os, sys
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = os.path.abspath("pyopenssl_test_env_" + os.path.basename(sys.executable))
NOSETESTS_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "nosetests"))
PYOPENSSL_DIR = os.path.abspath(os.path.join(ENV_NAME, "site-packages", "OpenSSL"))

packages = ["nose==1.3.7", "pycparser==2.13", "cryptography==1.0.1", "pyopenssl==0.15.1", "pyasn1==0.1.7", "idna==2.0", "six==1.9.0", "enum34==1.0.4", "ipaddress==1.0.14", "cffi==1.1.0"]
create_virtenv(ENV_NAME, packages, force_create = True)

expected = [{'ran': 247, 'errors': 2}]
run_test([NOSETESTS_EXE], cwd=PYOPENSSL_DIR, expected=expected)
