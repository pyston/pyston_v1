def f(a, b):
    print 'f is called', a, b

def g(a, b, c=3):
    print 'g is called', a, b, c

counter = 0

class Desc(object):
    def __get__(self, obj, type):
        global counter
        counter += 1
        if counter <= 800:
            return f
        else:
            return g

class C(object):
    d = Desc()

def do():
    c = C()
    c.d('a', 'b')

for i in xrange(1000):
    do()
