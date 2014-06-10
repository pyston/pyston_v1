# run_args: -n
# Trying to create an example of returning to a patchpoint that has been rewritten

class C(object):
    def f(self, n):
        print "C()", n
        f(n)
c = C()

def f(n):
    print "f()", n
    if n == 0 or n == 1:
        tocall = f
    elif n == 2:
        tocall = c.f
    else:
        return
    tocall(n + 1)
    print "done", n
f(0)
f(0)

class D(object):
    def __call__(self, n):
        print "D.__call__()", n
def call_f(c, n):
    c.f(n)
class C2(object):
    def f(self, n):
        print "f1", n
        C2.f = D()
        call_f(self, n)
c = C2()
call_f(c, 2)
call_f(c, 2)




n = 0

def f():
    C()

def init(self):
    pass

class C(object):
    def __new__(cls):
        global n
        n += 1

        if n == 10:
            C.__init__ = init
            f()

        return object.__new__(cls)

for i in xrange(20):
    f()
