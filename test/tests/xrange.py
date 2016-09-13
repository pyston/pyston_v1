# run_args: -n
#
# statcheck: "-L" in EXTRA_JIT_ARGS or 'slowpath_getclsattr' in stats or 'slowpath_callattr' in stats
# statcheck: noninit_count('slowpath_getclsattr') <= 20
# statcheck: noninit_count('slowpath_callattr') <= 22

for i in xrange(1000):
    print i

for i in xrange(10, 1, -1):
    print i

for i in xrange(10, -10, -3):
    print i

for i in xrange(0):
    print i

for i in xrange(10L, 15L, 1L):
    print i
