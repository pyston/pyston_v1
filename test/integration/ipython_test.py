import os
import sys
import subprocess


ENV_NAME = "ipython_test_env_{}".format(os.path.basename(sys.executable))


if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.join(os.path.dirname(__file__), "../lib/virtualenv/virtualenv.py")

    try:
        args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, ENV_NAME]
        print "Running", args
        subprocess.check_call(args)
        subprocess.check_call([ENV_NAME + "/bin/pip", "install"])
    except:
        print "Error occurred; trying to remove partially-created directory"
        ei = sys.exc_info()
        try:
            subprocess.check_call(["rm", "-rf", ENV_NAME])
        except Exception as e:
            print e
        raise ei[0], ei[1], ei[2]

IPYTHON_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../lib/ipython"))
PIP_EXE = os.path.abspath(os.path.join(ENV_NAME, "bin/pip"))

print "Installing IPython..."
subprocess.check_call([PIP_EXE, "install", "-e", ".[all]"], cwd=IPYTHON_DIR)

print "Running IPython test suite..."
subprocess.check_call([os.path.join(ENV_NAME, "bin/iptest")])

print
print 'PASSED'
