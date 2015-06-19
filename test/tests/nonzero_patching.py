# run_args: -n
# statcheck: noninit_count('slowpath_nonzero') <= 10

def f():
    for i in xrange(-10, 10):
        print i,
        if i:
            print "is truth-y"
        else:
            print "is false-y"
f()
