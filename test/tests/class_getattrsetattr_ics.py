# run_args: -n
# statcheck: noninit_count('slowpath_setattr') <= 10
# statcheck: noninit_count('slowpath_getattr') <= 10

class C(object):
    pass


c = C()
c.x = 0

for i in xrange(1000):
    c.x = c.x + 1
    print c.x
