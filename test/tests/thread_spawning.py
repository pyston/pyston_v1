# Make sure we can spawn a bunch of threads

import threading

cv = threading.Condition()

MAX_WORKERS = 2
nworkers = 0

def worker():
    global nworkers
    with cv:
        nworkers -= 1

        cv.notifyAll()

threads = []
for i in xrange(400):
    with cv:
        while nworkers >= MAX_WORKERS:
            cv.wait()

        nworkers += 1

    t = threading.Thread(target=worker)
    t.start()
    threads.append(t)

for t in threads:
    t.join()
