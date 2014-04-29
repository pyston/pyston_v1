# run_args: -n
#
# statcheck: "-O" in EXTRA_JIT_ARGS or 'slowpath_getclsattr' in stats or 'slowpath_callattr' in stats
# statcheck: stats.get('slowpath_getclsattr', 0) <= 20
# statcheck: stats.get('slowpath_callattr', 0) <= 20

for i in xrange(1000):
    print i
