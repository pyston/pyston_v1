# expected: fail
# - descriptors not implemented yet

class D(object):
    def __get__(self, instance, owner):
        print "get", instance, owner
        return 1

    def __set__(self, instance, value):
        print "set", instance, value

class C(object):
    d = D()

print C.d
print C().d

c = C()
c.d = 2
print c.d
