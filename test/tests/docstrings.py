print __doc__
__doc__ = "module_doc"
print __doc__

class C1(object):
    print 1, __doc__
    "hello world"
    print 2, __doc__
print C1.__doc__

class C2(object):
    "doc1"
    "doc2"
    print __doc__
print C2.__doc__

class C3(object):
    print __doc__
    pass
print "C3", C3.__doc__

class C4(object):
    print 1, __doc__
    "doc1"
    print 2, __doc__
    __doc__ = "doc2"
    print 3, __doc__
print C4.__doc__

class C5(object):
    __doc__ = "doc2"
print C5.__doc__

"""
# Not supported yet:

class C3(object):
    1
assert C3.__doc__ is None

class C4(object):
    ("a")
assert C3.__doc__ is None
"""
