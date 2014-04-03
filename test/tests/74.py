# Slice attributes should be visible

class C(object):
    def __getitem__(self, i):
        print i.start, i.step, i.stop
        # i.start = 1 # this should fail
        return i

c = C()
print c[:]
print c[::-1]
