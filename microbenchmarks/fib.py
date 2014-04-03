# Simple function call test.
# Uses this formulation (passing the function in to itself) to not require supporting scoped lookups.

def fib(n, f):
    if n <= 2:
        return n
    return f(n-1, f) + f(n-2, f)

print fib(36, fib)
