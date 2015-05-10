import os
import sys
import subprocess

me = sys.executable

with open('/dev/null')as ignore:
    def run(args):
        print subprocess.call([me] + args, stderr=ignore)

    # just prints out the usage
    run(["-m", "pydoc"])

    run(["-m", "doesnt_exist"])

    os.environ["PYTHONPATH"] = os.path.dirname(__file__)
    run(["-m", "import_target"])
