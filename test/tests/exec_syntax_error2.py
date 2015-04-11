# expected: fail
# - surprisingly, this is allowed?  doesn't seem very different from exec_syntax_error.py

# We print 1, CPython and PyPy print 2

s = """
def f():
    a = 1
    exec "a = 2" in None
    def g():
        print a
    g()
f()
"""
try:
    exec s
except Exception as e:
    print repr(e)
