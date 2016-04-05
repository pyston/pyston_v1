class C(object):
    def __init__(self):
        self.t = set([self])
    def __repr__(self):
        return repr(self.t)
print repr(C())
