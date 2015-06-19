# objmodel classattrs (like __add__) can be non-functions, so might not get bound into instancemethods:

class Adder(object):
    def __call__(self, *args):
        print args
        return 2

class C(object):
    __add__ = Adder()
    def __repr__(self):
        return "<C>"

c = C()
print c + c
