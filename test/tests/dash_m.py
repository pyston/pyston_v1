import os
import sys
import subprocess

me = sys.executable

with open('/dev/null')as ignore:
    def run(args):
        p = subprocess.Popen([me] + args, stdout = subprocess.PIPE, stderr=ignore)
        output, err = p.communicate()
        for num, line in enumerate(output.split('\n')):
            if num > 5:
                break
            print line
        print p.returncode

    # just prints out the usage
    run(["-Sm", "pydoc"])

    run(["-Sm", "doesnt_exist"])

    os.environ["PYTHONPATH"] = os.path.dirname(__file__)
    run(["-Sm", "import_target"])
