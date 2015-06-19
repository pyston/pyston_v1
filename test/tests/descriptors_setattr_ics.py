# run_args: -n
# statcheck: noninit_count('slowpath_setattr') <= 120

class Descriptor(object):
    def __set__(self, obj, value):
        print '__set__ called'
        print type(self)
        print type(obj)
        print type(value)

class C(object):
    a = Descriptor()

c = C()

def f(i):
    c.a = i

for i in xrange(1000):
    f(i)
