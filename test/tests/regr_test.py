import types
def f(self, args):
    if isinstance(args, types.StringTypes):
        pass


    try:
        self.pid
    except:
        pass


class C(object):
    pass
c = C()
c.pid = 1
for i in xrange(20000):
    f(c, None)
