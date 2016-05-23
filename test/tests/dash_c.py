import sys
import subprocess

me = sys.executable

with open('/dev/null')as ignore:
    # We don't (yet?) require exact stderr or return code compatibility w/
    # python. So we just check that we succeed or fail as appropriate.
    def run(args):
        code = 0 == subprocess.call([me] + args, stderr=ignore)
        sys.stdout.flush()
        print code
        sys.stdout.flush()

    run(["-Sc", "print 2 + 2"])
    run(["-Sc", "import sys; print sys.argv", "hello", "world"])
    run(["-Sc", "import sys; print sys.argv", "-c", "this is ignored"])

    run(["-Sc"])
    run(["-Sc", "-c"])
    run(["-Sc", "this should not work"])

    run(["-Sc", ";"])
    run(["-Scprint 1"])
