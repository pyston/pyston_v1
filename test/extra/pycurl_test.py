# pycurl depends on libcurl

import os
import sys
import subprocess
import shutil

p = subprocess.Popen(["which", "curl-config"], stdout=open("/dev/null", 'w'))
if p.wait() != 0:
    print >>sys.stderr, "curl-config not available; try 'sudo apt-get install libcurl4-openssl-dev'"
    sys.exit(1)

VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

if os.path.exists("pycurl_test_env"):
    print "Removing the existing 'pycurl_test_env/' directory"
    subprocess.check_call(["rm", "-rf", "pycurl_test_env"])
    # shutil follows symlinks to directories, and deletes whatever those contain.
    # shutil.rmtree("pycurl_test_env")

args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, "pycurl_test_env"]
print "Running", args
subprocess.check_call(args)

sh_script = """
set -e
. pycurl_test_env/bin/activate
set -ux
pip install pycurl==7.19.5.1
python -c 'import pycurl; print "pycurl imports"'
""".strip()

# print sh_script
subprocess.check_call(["sh", "-c", sh_script])

print
print "PASSED"
