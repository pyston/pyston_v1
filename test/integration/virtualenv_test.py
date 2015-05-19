import os
import sys
import subprocess
import shutil

VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/virtualenv/virtualenv.py"

if os.path.exists("test_env"):
    print "Removing the existing 'test_env/' directory"
    subprocess.check_call(["rm", "-rf", "test_env"])
    # shutil follows symlinks to directories, and deletes whatever those contain.
    # shutil.rmtree("test_env")

args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, "test_env"]
print "Running", args
subprocess.check_call(args)

sh_script = """
set -e
. test_env/bin/activate
set -ux
python -c 'import __future__'
python -c 'import sys; print sys.executable'
pip install bcrypt==1.1.0 python-gflags==2.0 sqlalchemy==1.0.0 Pillow==2.8.1
python -c 'import bcrypt; assert bcrypt.__version__ == "1.1.0"; assert bcrypt.hashpw("password1", "$2a$12$0123456789012345678901").endswith("I1hdtg4K"); print "bcrypt seems to work"'
python -c 'import gflags; print "gflags imports"'
python -c 'import sqlalchemy; print "sqlalchemy imports"'
python -c 'from PIL import Image; print "Pillow imports"'
""".strip()

# print sh_script
subprocess.check_call(["sh", "-c", sh_script], stdout=sys.stderr)

print
print "PASSED"
