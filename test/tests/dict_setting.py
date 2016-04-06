try:
    object().__dict__ = 1
except AttributeError as e:
    print e

class C(object):
    pass

c1 = C()
c2 = C()

c1.a = 2
c1.b = 3
c2.a = 4
c2.b = 5

def p():
    print sorted(c1.__dict__.items()), sorted(c2.__dict__.items())
    print sorted(list(iter(c1.__dict__))), sorted(list(iter(c2.__dict__)))

p()

c1.__dict__ = c2.__dict__
p()

c1.a = 6
c2.b = 7
p()

print c1.a, c1.b, c2.a, c2.b
del c1.a
try:
    del c1.a
except AttributeError as e:
    # the error message CPython gives here is just "a" which I don't think we should copy.
    print "caught AttributeError"
p()

c1.__dict__ = d = {}
d['i'] = 5
p()

class C(object):
    def foo(self):
        return 0
c = C()
c.attr = "test"
d1 = c.__dict__
d1.clear()
print hasattr(c, "attr")
print hasattr(c, "foo")
print c.__dict__ is d1

c = C()
a = c.__dict__
c.__dict__ = c.__dict__
c.__dict__[u"uni"] = "u"
c.__dict__[u"\u20ac"] = "u"
c.__dict__[(1, 2, 3)] = "t"
print sorted(c.__dict__.keys()), sorted(a.keys())
print "all non str attributes:", sorted([item for item in dir(c) if not isinstance(item, str)])
print "can we retrieve unicode attributes by there ascii value?", c.uni, c.__dict__["uni"]
print "attrwrapper:", a["uni"], a[(1, 2, 3)]
setattr(c, "uni", 1)
print "test setattr()", c.uni, c.__dict__["uni"], a["uni"], len(c.__dict__), len(a)

dictproxy = C.__dict__
print type(dictproxy)
print "foo" in dictproxy
print "foo" in dictproxy.keys()
print type(dictproxy["foo"])
print dictproxy == dict(C.__dict__)
try:
    dictproxy["test"] = "val"
except Exception as e:
    print e
