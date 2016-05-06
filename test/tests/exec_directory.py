import sys
import os
import subprocess

me = sys.executable
file_path = os.path.dirname(__file__)

with open('/dev/null')as ignore:
    def run(args):
        process = subprocess.Popen([me] + args,
                                   stdout=subprocess.PIPE,
                                   stderr=ignore)
        out, err = process.communicate()
        sys.stdout.flush()
        print(out)
        sys.stdout.flush()

    run(["".join([file_path, "/no_main_directory"])])
    run(["".join([file_path, "/no_main_directory/sub_dir"])])
