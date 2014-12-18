def call(f):
    return f()

def f():
    return 1

print call(f)
print call(int)
print int(1)
print int("2")

print int(1.1)
print int(1.5)
print int(1.9)
print int(-1.1)
print int(-1.5)
print int(-1.9)

print int(True)
print int(False)
