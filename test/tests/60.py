# len() and repr()

l = []
print len(l)
l.append(1)
print len(l)

print len("hello world")

print repr("hello 2")
print repr(1)
print repr
print repr(repr)
print str
print len
print str(str)

class C(object):
    def __init__(self, n):
        self.n = n
    def __len__(self):
        return self.n

print len(C(1))
try:
    print len(1)
except TypeError, e:
    print e
try:
    print len(C("hello world"))
except TypeError, e:
    print e
