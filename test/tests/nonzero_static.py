
# In certain cases, we can statically-determine that a variable is truthy.
# In the past, we would crash in irgen because of this, so for now this is just
# a crash test.

def f(x):
    print hasattr(x, "__nonzero__")
    if x:
        print "true"

# Objects without nonzero methods that are still true:
f(file)
f(open("/dev/null"))
f(Exception())
