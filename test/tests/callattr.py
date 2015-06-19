# run_args: -n
# callattr: c.foo(i) should theoretically create an instancemethod object and then call that,
# but calling an attribute is such a common case that we special case it as a callattr(),
# which avoids the allocation/immediate deallocation of the instancemethod object.
# statcheck: noninit_count('num_instancemethods') <= 10
# statcheck: noninit_count('slowpath_callattr') <= 120

class C(object):
    def foo(self, a0, a1, a2, a3, a4, a5, a6, a7):
        print "foo", a0, a1, a2, a3, a4, a5, a6, a7

    def baz(self, a0, a1):
        print "baz", a0, a1

def bar(a0, a1, a2, a3, a4, a5, a6, a7):
    print "bar", a0, a1, a2, a3, a4, a5, a6, a7

c = C()
c.bar = bar

for i in xrange(1000):
    c.bar(i, 100, i, 1, i, 2, i, 3)

for i in xrange(1000):
    c.baz(i, 100)

for i in xrange(1000):
    c.foo(i, 100, i, 1, i, 2, i, 3)
