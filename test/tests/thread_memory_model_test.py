from thread import start_new_thread
import time

a = 0
b = 0
done = []
def set_thread(num):
    global a, b
    print "starting set_thread", num
    for i in xrange(num):
        a += 1
        b += 1

        if i % 10000 == 0:
            print i
    done.append(None)

def check_thread(num):
    while b < num:
        _b = b
        _a = a
        assert _a >= _b, (_a, _b)
    done.append(None)

print "starting!"

N = 100000
start_new_thread(check_thread, (N,))
start_new_thread(set_thread, (N,))

while len(done) < 2:
    time.sleep(0)
