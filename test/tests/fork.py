# no-collect-stats

from thread import start_new_thread
import time
import os

counter = 0
done = 0
def daemon_thread():
    global counter
    while True:
        for i in xrange(100):
            counter += 1
        if done:
            counter = 0
            break
        time.sleep(0.0)

start_new_thread(daemon_thread, ())
while counter < 100:
    time.sleep(0.01)

def forceCollection():
    for i in xrange(100):
        [None] * 128000

pid = os.fork()
if pid:
    print "parent"

    r = os.waitpid(pid, 0)
    print r[1]
    print os.WIFSIGNALED(r[1]), os.WTERMSIG(r[1])
    assert os.WIFEXITED(r[1])
    assert os.WEXITSTATUS(r[1]) == 0
    print "parent waking up"

    forceCollection()

    done = True
    while counter:
        time.sleep(0.0)
    time.sleep(0.01)
else:
    print "child"
    forceCollection()
    print "child exiting successfully"
