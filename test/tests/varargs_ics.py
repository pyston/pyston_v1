# run_args: -n
# statcheck: 1 <= noninit_count("slowpath_runtimecall") + noninit_count("slowpath_runtimecall_capi") < 20

# Make sure we can patch some basic varargs cases

def f(*args):
    print sum(args)

def f2(a, b, c, d, e, *args):
    print sum(args)

for i in xrange(1000):
    f(10001)
    f(-1252, 1239)
    f(0, 839, 999)
    f2(1, 2, 3, 4, 5, 6, 7, 8)
