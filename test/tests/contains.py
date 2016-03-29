class C(object):
    def __init__(self, x):
        self.x = x

    def __eq__(self, y):
        print "__eq__", y
        return self.x

    def __contains__(self, y):
        print "__contains__", y
        return self.x

print 1 in C("hello") # "a in b" expressions get coerced to boolean
print 2 in C("")

print 1 in [C("hello")] # True
print 2 in [C("")] # False

for i in xrange(1, 4):
    print i in range(6), i not in range(5)

    print i in (1, 2, 5)

class D(object):
    def __getitem__(self, i):
        print i
        if i < 10:
            return i ** 2
        raise IndexError()
d = D()
print 5 in d
print 15 in d
print 25 in d

class D():
    def __getitem__(self, i):
        print i
        if i < 10:
            return i ** 2
        raise IndexError()

d = D()
print 5 in d
print 15 in d
print 25 in d

class F():
    def __init__(self):
        self.n = 0

    def __iter__(self):
        return self

    def next(self):
        if self.n >= 10:
            raise StopIteration()
        self.n += 1
        return self.n ** 2
f = F()
print 5 in f
print 15 in f
print 25 in f
