# run_args: -n
# Tests to make sure that we guard on enough things for instance methods:

class C(object):
    def foo(self):
        print "C.foo()"
    def bar(self):
        print "C.bar()"
    def baz(self, a, b, c, d, e, f, g, h, i):
        print "C.baz()"
    def faz(self, a, b, c, d, e, f, g, h, i):
        print "C.faz()"
class D(object):
    def foo(self):
        print "D.foo()"
    def bar(self):
        print "D.bar()"
    def baz(self, a, b, c, d, e, f, g, h, i):
        print "D.baz()"
    def faz(self, a, b, c, d, e, f, g, h, i):
        print "D.faz()"

c = C()
d = D()
l = [c.foo, c.bar, d.foo, d.bar,
        c.foo, d.foo, c.bar, d.bar]

for i in l:
    i()

l = [c.faz, c.baz, d.faz, d.baz,
        c.faz, d.faz, c.baz, d.baz]
for i in l:
    i(1, 2, 3, 4, 5, 6, 7, 8, 9)

