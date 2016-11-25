class C(object):
    pass

c = C()
print c is c
print c is C

print range is True
print c is None

print 1 is 1.0
print 1.0 is 1
print 1 is True
print 0 is False
print True is 1

# they don't have to be True but Pyston implements it like this for perf reasons
print
print 1 is 1
print 1.0 is 1.0
print 1l is 1l
print "s" is "s"
print u"u" is u"u"
