# expected: fail
# - I'm not sure how this is supposed to work

# I guess the __name__ attribute on classes is weird?

class C(object):
    pass

print C.__name__
print C.__module__
# print C.__dict__.keys() # __name__ doesn't appear!
# print dir(C) # __name__ doesn't appear!
c = C()

print c.__name__ # this should err
