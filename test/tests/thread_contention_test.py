# skip-if: True
# - Pyston is not yet thread safe.  this test sometimes works and sometimes doesn't.
# - threads also seem to cause much greater memory usage for both CPython and Pyston and
#   blow out the memory limits set by the tester.

from thread import start_new_thread
import time

work = []
done = []
def run(num):
    for i in xrange(num):
        t = work.pop()
        work.append(t - 1)
    done.append(num)

print "starting!"

nthreads = 2
for i in xrange(nthreads):
    work.append(1000000)
for i in xrange(nthreads):
    t = start_new_thread(run, (1000000,))

while len(done) < nthreads:
    time.sleep(0)

print work, sum(work)
