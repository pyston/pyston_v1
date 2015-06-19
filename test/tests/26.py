# run_args: -n
# statcheck: noninit_count("slowpath_runtimecall") <= 10
# statcheck: stats.get("slowpath_callclfunc", 0) <= 5
# Simple patchpoint test:

def f():
    def p(a, b, c, d):
        print a, b, c, d

    y = 100
    while y:
        p(1, y, 2, 3)
        y = y - 1

    y = 100
    while y:
        p(1, y, "hello", 5)
        y = y - 1
f()

