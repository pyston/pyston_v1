# Re-patching test: have a single callsite that frequently has different target functions:

def a():
    print "a"
    return 1

def b():
    print "b"
    return 2

def c(f):
    # The difficult callsite:
    return f()

x = 100
y = 0
while x:
    y = y + c(a)
    print y
    y = y + c(b)
    print y
    x = x - 1

print y
