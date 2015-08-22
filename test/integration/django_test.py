import os
import signal
import subprocess
import sys
import time
import urllib2

EXTRA_PATH = os.path.dirname(os.path.abspath(__file__)) + "/../lib/django"
sys.path.insert(0, EXTRA_PATH)

from django.core.management import execute_from_command_line

import os
import shutil

is_pyston = True
if is_pyston:
    ARGS = "-u"
else:
    ARGS = "-u"

if os.path.exists("testsite"):
    print "Removing the existing 'testsite/' directory"
    shutil.rmtree("testsite")

try:
    sys.argv += ["startproject", "testsite"]
    print "Running 'startproject testsite'"
    r = execute_from_command_line()
    assert not r

    # In theory we could run this in the current process (migrate.py is only a couple lines),
    # but I guess the "startproject testsite" command changed enough global state that
    # it won't work.  So create a new subprocess to run it instead.
    print "Running testsite/manage.py migrate"
    env = dict(os.environ)
    env["PYTHONPATH"] = env.get("PYTHONPATH", "") + ":" + EXTRA_PATH
    subprocess.check_call([sys.executable, ARGS, "testsite/manage.py", "migrate"], env=env)

    print "Running runserver localhost:8000"
    p = subprocess.Popen([sys.executable, ARGS, "testsite/manage.py", "runserver", "--noreload", "localhost:8000"], stdout=subprocess.PIPE, env=env)

    try:
        print "Waiting for server to start up"
        while True:
            l = p.stdout.readline()
            assert l, "unexpected eof"
            print l

            if l.startswith("Quit the server with CONTROL-C"):
                break

        # Give the server some extra time to start up:
        time.sleep(1)

        print "Server started up, fetching home page"
        f = urllib2.urlopen("http://localhost:8000/", timeout=1)
        s = f.read()
        assert "Congratulations on your first Django-powered page" in s

        print "Shutting down server"
        # ctrl-C is how you shut down the django development server cleanly, but Pyston doesn't yet support that.
        # So you'll see a "SIGINT! someone called about" message and then a traceback on stderr.
        p.send_signal(signal.SIGINT)
        while True:
            l = p.stdout.readline()
            if not l:
                break
            assert "Error" not in l, l
            print l
        code = p.wait()
        # We can enable this assert once we support signals such as ctrl-C:
        # assert code == 0
    except:
        p.kill()
        p.wait()
        raise
finally:
    pass
    # shutil.rmtree("testsite")

print
print "PASSED"
