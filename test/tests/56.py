# expected: fail
# - arbitrary stuff in classes

# I guess type.__name__ works specially:

class C(object):
    __name__ = 1
print C.__name__
c = C()
print c.__name__
