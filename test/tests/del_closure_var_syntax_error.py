try:
    import __pyston__
    __pyston__.setOption("LAZY_SCOPING_ANALYSIS", 0)
except ImportError:
    pass

cases = [

"""
# should fail
def f():
    a = 0
    def g():
        print a
    del a
""", """
# should fail
def f():
    def g():
        print a
    del a
""", """
def f():
    global a
    a = 0
    def g():
        print a
    del a
""", """
def f():
    a = 0
    def g():
        global a
        print a
    del a
""", """
def f():
    a = 0
    def g():
        global a
        def h():
            print a
    del a
""", """
def f():
    class C(object):
        a = 0
        del a
        def g():
            print a
"""

]

#import traceback

for case in cases:
    print case
    try:
        exec case
    except SyntaxError as se:
        print se.message
