import os
import sys
import subprocess
import shutil

ENV_NAME = "pyxl_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/virtualenv/virtualenv.py"

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]


PYXL_DIR = os.path.dirname(__file__) + "/pyxl"
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

subprocess.check_call([python_exe, "setup.py", "build"], cwd=PYXL_DIR)
subprocess.check_call([python_exe, "setup.py", "install"], cwd=PYXL_DIR)
subprocess.check_call([python_exe, "finish_install.py"], cwd=PYXL_DIR)
out = subprocess.check_output([python_exe, "pyxl/examples/hello_world.py"], cwd=PYXL_DIR)

print

print "Output: '%s'" % out
assert out == "<html><body>Hello World!</body></html>\n"

subprocess.check_call(["rm", "-rf", os.path.join(PYXL_DIR, "build")])

print "PASSED"
