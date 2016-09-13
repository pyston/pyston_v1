# skip-if: '-n' in EXTRA_JIT_ARGS or '-L' in EXTRA_JIT_ARGS

# Make sure that we can fork from a threaded environment
#
# Running this test with -n or -L has horrible performance, since
# we will freshly JIT all of the post-fork code after every fork.

import subprocess
import threading

print_lock = threading.Lock()

def worker(id):
    for i in xrange(100):
        subprocess.check_call(["true"])
    with print_lock:
        print "done"

threads = []
for i in xrange(4):
    t = threading.Thread(target=worker, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()
