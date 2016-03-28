import signal

for k in sorted(dir(signal)):
    if not k.startswith("SIG"):
        continue
    print k, getattr(signal, k)

print hasattr(signal, "alarm")


import time
import signal

def sig_handler(signum, stack):
    print "inside sig_handler"
    import sys, traceback
    traceback.print_stack(stack)
    sys.exit(0)

def f(lst):
    signal.signal(signal.SIGALRM, sig_handler)
    signal.setitimer(signal.ITIMER_REAL, 2, 1)
    for x in lst:
        time.sleep(x)  #1
        time.sleep(x)  #2

f([0] * 100 + [10])
assert False, "shuld not get executed"
