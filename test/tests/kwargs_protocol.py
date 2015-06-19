class C(object):
    def __len__(self):
        print "__len__"
        return 2

    def __iter__(self):
        print "__iter__"
        return self

    def next(self):
        print "Next"
        raise StopIteration()

    def __getitem__(self, k):
        print "getitem", k
        return k

    def keys(self):
        print "keys"
        return ["a", "c", "b"]

def f(a, b, c):
    print a, b, c

f(**C())


class MyDict(dict):
    pass

d = MyDict(a=1, b=2, c=3)
print f(**d)


# Django does this:
class C(object):
    pass
c = C()
c.a = 1
c.b = 3
c.c = 7
print f(**c.__dict__)
