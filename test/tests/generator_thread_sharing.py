# Start a generator on one thread, pass it to another, and have that execute it for a while

def gen():
    while True:
        for i in xrange(100):
            range(100) * 1000 # allocate memory to force GC collections
        b = yield 0
        if b:
            break

done = []
def thread_run(g):
    for i in xrange(5):
        g.next()
    try:
        g.send(1)
        assert 0
    except StopIteration:
        pass
    done.append(0)

g = gen()
for i in xrange(5):
    g.next()

from thread import start_new_thread
t = start_new_thread(thread_run, (g,))
g = ""

def f():
    pass

f()
f()

import time
while not done:
    time.sleep(0.1)
