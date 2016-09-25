from _weakref import ref

# Test to make sure that we clear local variables at the right time:
def f1():
    def f():
        pass
    r = ref(f)
    print type(r())
    # del f
    f = 1
    assert not r()
for i in xrange(40):
    f1()

def f3():
    class MyIter(object):
        def __init__(self):
            self.n = 5
        def __del__(self):
            print "deleting iter"
        def next(self):
            if self.n:
                self.n -= 1
                return self.n
            raise StopIteration()

    class MyIterable(object):
        def __iter__(self):
            return MyIter()

    for i in MyIterable():
        print i
    else:
        print -1

    for i in MyIterable():
        break
    print "a"
f3()
