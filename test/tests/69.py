# should_error
class C(object):
    pass

print C.__name__
print C.__module__
c = C()

print c.__name__ # this should err
