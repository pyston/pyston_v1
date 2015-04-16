# run_args: -x
# - pypa currently has a couple issues with django (set literals with trailing commas, nested attribute names)
# TODO remove that directive, and also remove it from the subprocess commands down below.

import os
import subprocess
import sys

EXTRA_PATH = os.path.dirname(__file__) + "/django"
sys.path.insert(0, EXTRA_PATH)

from django.core.management import execute_from_command_line

import os
import shutil

if os.path.exists("testsite"):
    print "Removing the existing 'testsite/' directory"
    shutil.rmtree("testsite")

try:
    sys.argv += ["startproject", "testsite"]
    print "Running 'startproject testsite'"
    r = execute_from_command_line()
    assert not r

    print "Running testsite/manage.py migrate"
    env = dict(os.environ)
    env["PYTHONPATH"] = env.get("PYTHONPATH", "") + ":" + EXTRA_PATH
    subprocess.check_call([sys.executable, "-x", "testsite/manage.py", "migrate"], env=env)
    # subprocess.check_call([sys.executable, "testsite/manage.py", "runserver", "--noreload"])

finally:
    pass
    # shutil.rmtree("testsite")
