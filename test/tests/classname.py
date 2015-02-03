# expected: fail

# This fails becasue we currently don't support setting for getset descriptors,
# and __name__ is a getset descriptor.

class C(object):
    pass

print C
C.__name__ = "new_name"
print C
print repr(C.__module__)
C.__module__ = "new_module"
print C

C.__module__ = 1
print C
