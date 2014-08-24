__builtins__.aoeu = 1
print aoeu

__builtins__.True = 2
print True
print bool(1)
print bool(1) is True

__builtins__.__builtins__ = 1
print __builtins__

__builtins__ = 2
print __builtins__

print all([]), all([True]), all([False]), all([None]), all([True, False, None])
print any([]), any([True]), any([False]), any([None]), any([True, False, None])

print sum(range(5))
print sum(range(5), 5)

class C(object):
    def __init__(self, n):
        self.n = n
    def __add__(self, rhs):
        self.n = (self.n, rhs.n)
        return self

print sum([C(1), C(2), C(3)], C(4)).n

print zip([1, 2, 3, 0], ["one", "two", "three"])

print filter(lambda x: x % 2, xrange(20))
print type(enumerate([]))
print list(enumerate(xrange(5, 10)))

# If the first argument is None, filter calls checks for truthiness (ie is equivalent to passing 'bool')
print filter(None, xrange(-5, 5))

print isinstance(1, int)
print isinstance(1, (float, int))
print isinstance(1, (float, (), (int, 3), 4))
