class C1(object):
    "hello world"
print C1.__doc__

class C2(object):
    "doc1"
    "doc2"
print C2.__doc__

class C3(object):
    pass
print C3.__doc__

"""
# Not supported yet:

class C3(object):
    1
assert C3.__doc__ is None

class C4(object):
    ("a")
assert C3.__doc__ is None
"""
