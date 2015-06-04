from testing_helpers import test_gc

# __del__ does not get called because it doesn't fallback to getattr

# Note that this is an old-style class.
class C:
    def __getattr__(self, name):
        def foo():
            return 0
        print name
        return foo

def foo():
    c = C()
    l = range(10)

    # This should cause __index__ to be printed because it fallbacks to getattr
    l[c] = 1

    # Here, c goes out of scope.
    return

test_gc(foo)
