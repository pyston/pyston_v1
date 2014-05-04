# expected: fail
# - exceptions

class D(object):
    def __getitem__(self, idx):
        print "getitem", idx

        if idx >= 20:
            raise IndexError()
        return idx

print 10 in D()
print 1000 in D()


