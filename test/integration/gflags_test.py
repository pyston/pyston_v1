import glob
import subprocess, sys, os

gflags_dir = os.path.dirname(os.path.abspath(__file__)) + "/../lib/gflags"
os.chdir(gflags_dir)

env = os.environ
env["PYTHONPATH"] = "."

TESTS_DIR = "tests"
for fn in glob.glob("%s/*.py" % TESTS_DIR):
    # We don't support xml.dom.minidom yet
    if "helpxml_test.py" in fn:
        print "Skipping", fn
        continue

    print "Running", fn
    subprocess.check_call([sys.executable, fn])

print "-- Tests finished"
