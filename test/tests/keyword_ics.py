# expected: statfail
# - rewriter bails on keywords for now

# statcheck: stats['slowpath_runtimecall'] <= 20
# statcheck: stats.get("slowpath_callclfunc", 0) <= 20
# statcheck: stats['rewriter_nopatch'] <= 20
def f(a, b):
    print a, b

for i in xrange(10000):
    f(a=1, b=2)
    f(b=1, a=2)
