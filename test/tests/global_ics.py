# run_args: -n
# statcheck: noninit_count("slowpath_getglobal") <= 10

def f():
    print True

True = True

n = 1000
while n:
    f()
    n = n - 1
