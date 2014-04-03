# run_args: -n
# statcheck: stats["slowpath_getglobal"] <= 10

def f():
    print True

True = True

n = 1000
while n:
    f()
    n = n - 1
