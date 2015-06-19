class C(object):
    def __repr__(self):
        print "repr"
        return 1

c = C()
print "C object" in object.__repr__(c)
print object.__str__(c)

print c.__repr__()
print c.__str__()
