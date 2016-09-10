# Make sure we can recurse at least 900 times on the three different types
# of stacks that we have:

import sys
TEST_DEPTH = sys.getrecursionlimit() - 20

def recurse(n):
    if n > 0:
        return recurse(n - 1)
    return n

print "Recursing on main thread..."
recurse(TEST_DEPTH)

print "Recursing in a generator..."
def gen():
    yield recurse(TEST_DEPTH)
print list(gen())

print "Recursing in a thread..."
from thread import start_new_thread
import time

done = 0
def thread_target():
    global done

    recurse(TEST_DEPTH)

    done = 1
start_new_thread(thread_target, ())

while not done:
    time.sleep(0.001)

s = """
if depth < TEST_DEPTH:
    depth += 1
    exec s
"""
exec s in {'depth': 0, 's': s, 'TEST_DEPTH': TEST_DEPTH}
