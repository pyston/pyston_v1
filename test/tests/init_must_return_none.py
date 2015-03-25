# should_error
# As the test filename says, init functions must return None.
# This file tests that; it also makes sure that it gets tested
# when in a patchpoint.

class C(object):
    def __init__(self, n):
        self.n = n
        if n == 500:
            return n

# Call it in a loop to make sure that the constructor gets inlined:
for i in xrange(1000):
    c = C(i)
    print c.n
