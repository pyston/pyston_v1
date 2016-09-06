import os
import sys
import subprocess
import shutil

VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

ENV_NAME = "test_env_" + os.path.basename(sys.executable)

if os.path.exists(ENV_NAME):
    print "Removing the existing '%s/' directory" % ENV_NAME
    subprocess.check_call(["rm", "-rf", ENV_NAME])
    # shutil follows symlinks to directories, and deletes whatever those contain.
    # shutil.rmtree(ENV_NAME)

args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
print "Running", args
subprocess.check_call(args)

sh_script = ("""
set -e
. %s/bin/activate
set -ux
python -c 'import __future__'
python -c 'import sys; print sys.executable'
# The first entry of each line is the main thing we're testing; the rest are dependencies.
# List the dependencies explicitly so that we can enforce specific revisions; these were the
# versions that got installed as of 6/5/15
pip install bcrypt==1.1.0 cffi==1.1.0 six==1.9.0 pycparser==2.13
pip install python-gflags==2.0
pip install sqlalchemy==1.0.0
pip install Pillow==2.8.1
pip install decorator==3.4.2
pip install oauth2client==1.4.11 httplib2==0.9.1 pyasn1==0.1.7 pyasn1-modules==0.0.5 rsa==3.1.4
python -c 'import bcrypt; assert bcrypt.__version__ == "1.1.0"; assert bcrypt.hashpw("password1", "$2a$12$0123456789012345678901").endswith("I1hdtg4K"); print "bcrypt seems to work"'
python -c 'import gflags; print "gflags imports"'
python -c 'import sqlalchemy; print "sqlalchemy imports"'
python -c 'from PIL import Image; print "Pillow imports"'
python -c 'import decorator; print "decorator imports"'
python -c 'import oauth2client; print "oauth2client imports"'
""" % ENV_NAME).strip()

# print sh_script
subprocess.check_call(["sh", "-c", sh_script], stdout=sys.stderr)

print
print "PASSED"
