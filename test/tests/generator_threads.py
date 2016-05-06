# Pass a started generator to two different threads, and make them both
# try to run it at the same time.

started = []
def gen():
    started.append(None)
    while len(started) == 1:
        pass

    yield "done"

done = []
def run_through(g, i):
    if i == 0:
        print g.next()
    else:
        while len(started) < 1:
            pass

        try:
            print g.next()
        except ValueError, e:
            print e

        started.append(None)

    done.append(None)

g = gen()
from thread import start_new_thread

start_new_thread(run_through, (g, 0))
start_new_thread(run_through, (g, 1))

import time
while len(done) < 2:
    time.sleep(0.01)
