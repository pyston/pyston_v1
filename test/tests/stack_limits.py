# Make sure we can recurse at least 900 times on the three different types
# of stacks that we have:

def recurse(n):
    if n > 0:
        return recurse(n - 1)
    return n

print "Recursing on main thread..."
recurse(900)

print "Recursing in a generator..."
def gen():
    yield recurse(900)
print list(gen())

print "Recursing in a thread..."
from thread import start_new_thread
import time

done = 0
def thread_target():
    global done

    recurse(900)

    done = 1
start_new_thread(thread_target, ())

while not done:
    time.sleep(0.001)
