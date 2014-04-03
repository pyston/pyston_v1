# run_args: -n
# statcheck: stats['slowpath_setattr'] <= 10
# statcheck: stats['slowpath_getattr'] <= 10

class C(object):
    pass


c = C()
c.x = 0

for i in xrange(1000):
    c.x = c.x + 1
    print c.x
