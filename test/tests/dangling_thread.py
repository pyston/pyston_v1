# skip-if: True
# - I don't think this test is reliable on CPython, even if we use the threading module

# Make sure that we can exit with threads running in the background.

from thread import start_new_thread
import time

counter = 0
def daemon_thread():
    global counter
    while True:
        for i in xrange(100):
            counter += 1
        time.sleep(0.0)

start_new_thread(daemon_thread, ())
while counter < 100:
    time.sleep(0.01)

print "exiting"
# exit without stopping the daemon thread
