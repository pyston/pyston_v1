# run_args: -n
# This is a test to make sure that clsattrs get incref'd for the duration of their execution.
# It feels pretty wasteful but theoretically the clsattr could clear itself (or indirectly)
# which would dealloc it while running if it wasn't incref'd.

# I'm not sure this test would actually crash though, since we're not freeing the code
# memory...

class C(object):
    def f(self):
        C.f = None
        # random stuff to try to make it crash:
        l = [1,2,3]
        while l:
            l.pop()

print C.f is None
C().f()
print C.f is None
