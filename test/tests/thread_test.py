from thread import start_new_thread, allocate_lock, _count
import time

print type(allocate_lock())

print_lock = allocate_lock()

done = 0
def run(arg):
    global done
    with print_lock:
        print "num threads:", _count()
        print "in other thread!", arg
    done = 1


print "starting!"
print "num threads:", _count()
with print_lock:
    t = start_new_thread(run, (5,))
    print type(t)

while not done:
    time.sleep(0)

print "done!"
print "num threads:", _count()

done = False
with print_lock:
    t = start_new_thread(run, (), {'arg': 6})
while not done:
    time.sleep(0)

l = allocate_lock()
print "locked:", l.locked()
print l.acquire()
print "locked:", l.locked()
print l.acquire(0)
print "locked:", l.locked()
print l.release()
print "locked:", l.locked()
print l.acquire(0)


lock = allocate_lock()
state = 0
def run2():
    global state

    print lock.acquire(0)
    print "num threads:", _count()
    state = 1
    print lock.acquire()
    lock.release()
    state = 2

with lock:
    start_new_thread(run2, ())

    while state != 1:
        time.sleep(0)
while state != 2:
    time.sleep(0)

print "done!"
