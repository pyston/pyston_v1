# Objects are iterable if they just implement __getitem__:

class C(object):
    def __getitem__(self, i):
        print "getitem", i
        return range(10)[i]

print type(iter(C()))

for i in C():
    print i
