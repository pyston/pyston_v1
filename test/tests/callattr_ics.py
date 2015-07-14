# run_args: -n
# statcheck: noninit_count('slowpath_callattr') <= 200

def f(a, b):
    print 'f is called', a, b

class Desc(object):
    def __get__(self, obj, type):
        return f

class C(object):
    d = Desc()

def do():
    c = C()
    c.d('a', 'b')

for i in xrange(1000):
    do()
