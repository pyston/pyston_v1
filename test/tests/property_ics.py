# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 25

class C(object):
    @property
    def prop(self):
        return 42
c = C()
for i in xrange(1000):
    a = c.prop
