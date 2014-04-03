class C(object):
    def __str__(self):
        return "str"
    def __repr__(self):
        return "repr"

print C()

print int(1)
print repr(1)
print str("hi")
print repr("hi")
