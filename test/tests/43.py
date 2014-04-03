# icsetattr-existingattr test

def f():
    pass

n = 10
while n:
    n = n - 1
    f.x = n
    print f.x
