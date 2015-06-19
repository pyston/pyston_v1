#

# currently broken:
# import os.path

import os

r1 = os.urandom(8)
r2 = os.urandom(8)
print len(r1), len(r2), type(r1), type(r2), r1 == r2

print type(os.stat("/dev/null"))

print os.path.expanduser("~") == os.environ["HOME"]

print os.path.isfile("/dev/null")
print os.path.isfile("/should_not_exist!")

e = OSError(1, 2, 3)
print e
print e.errno
print e.strerror
print e.filename
print OSError(1, 2).filename

try:
    os.execvp("aoeuaoeu", ['aoeuaoeu'])
except OSError, e:
    print e

# Changes to os.environ should show up in subprocesses:
import subprocess
env = os.environ
env["PYTHONPATH"] = "."
subprocess.check_call("echo PYTHONPATH is $PYTHONPATH", shell=1)
