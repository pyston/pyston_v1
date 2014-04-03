# Re-patching test: have a single callsite that frequently has different target functions:

def a():
    return 1

def b():
    return 2

def c(f):
    # The difficult callsite:
    return f()

x = 1000000
y = 0
while x:
    y = y + c(a)
    y = y + c(b)
    x = x - 1

print y
