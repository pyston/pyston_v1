# nested OSR test
# While in the outer invocations (level=0 or 1), the inner-most invocation will cause
# an OSR recompilation.
# The outer versions should be able to join the optimized version once they decide
# to osr as well.
# Actually, I'm not so sure what this actually ends up testing

def f(level, f):
    if level == 0:
        n = 10010
    else:
        n = 10

    k = 0
    while n:
        if level <= 2:
            k = k + f(level + 1, f)
        n = n - 1
        k = k + n
    return k
print f(0, f)
