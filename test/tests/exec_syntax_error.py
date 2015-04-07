
s = """
def f():
    a = 1
    exec "a = 2"
    def g():
        print a
    g()
f()
"""
try:
    exec s
except Exception as e:
    print repr(e)

s = """
def f():
    a = 1
    d = {}
    exec "a = 2" in {}, d
    def g():
        print a
    g()
    print d
f()
"""
try:
    exec s
except Exception as e:
    print repr(e)
