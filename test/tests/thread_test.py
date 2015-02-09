from thread import start_new_thread, allocate_lock
import time

print type(allocate_lock())

done = 0
def run(arg):
    global done
    print "in other thread!", arg
    done = 1


print "starting!"
t = start_new_thread(run, (5,))
print type(t)

while not done:
    time.sleep(0)

print "done!"

l = allocate_lock()
print l.acquire()
print l.acquire(0)
print l.release()
print l.acquire(0)


lock = allocate_lock()
state = 0
def run2():
    global state

    print lock.acquire(0)
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
