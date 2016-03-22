# expected: reffail
def f():
    exec "a = 5"
    print a
f()
