import os
import sys
import subprocess
import time

ENV_NAME = "pyinotify_test_env_" + os.path.basename(sys.executable)

if not os.path.exists(ENV_NAME) or os.stat(sys.executable).st_mtime > os.stat(ENV_NAME + "/bin/python").st_mtime:
    print "Creating virtualenv to install testing dependencies..."
    VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

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


PYINOTIFY_DIR = os.path.dirname(__file__) + "/../lib/pyinotify"
python_exe = os.path.abspath(ENV_NAME + "/bin/python")

print "\nRunning pyinotify\n"

script = """
import pyinotify
import sys

wm = pyinotify.WatchManager()  # Watch Manager
mask = pyinotify.IN_DELETE | pyinotify.IN_CREATE  # watched events

output = []

class EventHandler(pyinotify.ProcessEvent):
    def process_IN_CREATE(self, event):
        output.append("Changing: " + event.pathname)

    def process_IN_DELETE(self, event):
        output.append("Changing: " + event.pathname)
        if len(output) >= 2:
            for line in output:
                sys.stdout.write(line)
                sys.stdout.write("\\n")
            sys.stdout.close()
            sys.exit()

handler = EventHandler()
notifier = pyinotify.Notifier(wm, handler)
wdd = wm.add_watch('/tmp', mask, rec=True)

notifier.loop()
"""

p = subprocess.Popen(["python", "-c", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# Give pyinotify some extra time to start up:
time.sleep(1)

# Create a file create and a file delete event
subprocess.call(["touch", "/tmp/generate_file_event_for_pyinotify"])
subprocess.call(["rm", "/tmp/generate_file_event_for_pyinotify"])

# Veiry pyinotify watched the file create and delete
while True:
    line = p.stdout.readline()
    if line != '':
        assert "Changing" in line
        print line.rstrip()
    else:
        break

print
print "PASSED"
