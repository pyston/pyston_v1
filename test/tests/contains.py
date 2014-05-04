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

