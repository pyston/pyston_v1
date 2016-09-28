import os, sys, subprocess, shutil
sys.path.append(os.path.dirname(__file__) + "/../lib")

from test_helper import create_virtenv, run_test

ENV_NAME = "typing_test_env_" + os.path.basename(sys.executable)
ENV_DIR = os.path.abspath(ENV_NAME)
PYTHON_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin", "python"))

pkg = ["typing==3.5.2.2"]
# pkg = ["git+https://github.com/python/typing.git@3.5.2.2"]

create_virtenv(ENV_NAME, pkg, force_create=False)

# subprocess.check_call([PYTHON_EXE, os.path.join(ENV_DIR, "lib", "python2.7", "site-packages", "typing", "test_typing.py")])
# subprocess.check_call([PYTHON_EXE, "-m", "test_typing"])

print "Running some extra tests:"
test_fn = os.path.join(ENV_DIR, "test.py")
with open(test_fn, 'w') as f:
    f.write( """
import sys

from typing import Generic, TypeVar, Sequence
RR = TypeVar('RR')

for i in xrange(1000):
    Generic[RR]
    Sequence[RR]

print "Passed"
""")
subprocess.check_call([PYTHON_EXE, test_fn])

print "Running typing's own tests:"
import urllib
test_file = urllib.urlopen("https://raw.githubusercontent.com/python/typing/1e4c0bae6f797ee5878ce4bb30f3b03c679e3e11/python2/test_typing.py").read()
test_fn = os.path.join(ENV_DIR, "test_typing.py")
with open(test_fn, 'w') as f:
    f.write(test_file)
subprocess.check_call([PYTHON_EXE, test_fn])

