o = object()
r = o.__reduce__()
print type(r), len(r)

class C(object):
    def __repr__(self):
        return "<C>"
c = C()
r = c.__reduce__()
print type(r), len(r)
assert len(r) == 2
c2 = r[0](*r[1])

print c, c2
