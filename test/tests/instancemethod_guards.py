# Test what happens when we swap out a function (which we handle special-cased
# in the descriptor logic) for a descriptor of another type
# TODO should be more thorough
# TODO should write similar tests for MemberDescriptors too

def g():
    print 'in g'

class Descriptor(object):
    def __str__(self):
        return 'Descriptor'
    def __get__(self, obj, objtype):
        return g

class C(object):
    def f(self):
        print 'in f'

c = C()

def run():
    c.f()

    t = c.f
    t()

for i in xrange(100):
    run()

    if i == 50:
        C.f = Descriptor()

# Swap an unbound for a bound

class C(object):
    def f(self, a=0):
        print "f", a
    def __str__(self):
        return "C obj"

def call(f, c):
    #TODO uncomment this
    #print f
    f(c)

c = C()
call(C.f, c)
call(c.f, c)
