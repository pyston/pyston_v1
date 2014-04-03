# Same as fib.py, but I now support global lookups!

def fib(n):
    if n <= 2:
        return n
    return fib(n-1) + fib(n-2)

print fib(36)
