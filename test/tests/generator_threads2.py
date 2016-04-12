import threading
import traceback, sys

def exc():
    1/0

def G():
    traceback.print_stack(limit=2)
    yield 1
    traceback.print_stack(limit=2)
    yield 2
    exc()

def f1(x):
    print x.next()
def f2(x):
    print x.next()
def f3(x):
    try:
        print x.next()
    except:
        print "exc"
        traceback.print_tb(sys.exc_info()[2])
def run(nthreads=4):
    g = G()
    def t(f):
        return threading.Thread(target=f, args=(g,))
    for t in [t(f1), t(f2), t(f3)]:
        t.start()
        t.join()
run()
