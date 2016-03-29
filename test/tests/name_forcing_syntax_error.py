# We could just have a file for each, but if we Do these in execs,
# we don't need separate files for each, and that makes it easier
# to just spam all the permutations.

# The logic beyond this error message is oddly complicated.

try:
    import __pyston__
    __pyston__.setOption("LAZY_SCOPING_ANALYSIS", 0)
except ImportError:
    pass

cases = [

# protip: delete this first """ to get your editor to syntax-highlight the code

"""

# This should compile fine
def f():
    a = 0
    exec "b = 0"

""", """
# This should compile fine
def addpackage(sitedir, name, known_paths):
    print a
    exec "b = 0"

""", """

def f():
    exec "a = 5"
    def g():
        print a
""", """

def f():
    exec "a = 5"
    def g():
        def h():
            print a

""", """

def f():
    exec "a = 5"
    class C(object):
        def h():
            print a
""", """

def f():
    exec "a = 5"
    def g():
        b = 2
        def h():
            print b
""", """

def f():
    def g():
        print a
        exec "a = 5"
""", """

def f():
    from string import *
    def g():
        print a
""", """

def f():
    from string import *
    def g():
        def h():
            print a
""", """

def f():
    from string import *
    class C(object):
        def h():
            print a
""", """

def f():
    from string import *
    def g():
        b = 2
        def h():
            print b
""", """

def f():
    def g():
        print a
        from string import *
""", """

def f():
    exec "a = 5"
    from string import *
    def g():
        print a
""", """

def f():
    from string import *
    exec "a = 5"
    def g():
        print a
""", """

def f():
    def g():
        print a
        from string import *
        exec "a = 5"
""", """

def f():
    def g():
        print a
        exec "a = 5"
        from string import *

""", """

def f():
    def g():
        exec "a = 5"

""", """

class C(object):
    def g():
        a = 5
        exec "a = 5"

""", """

class C(object):
    def g():
        exec "a = 5"

""", """
def f():
    exec "a = 5"
    return {b for b in xrange(3)}
""", """
def f():
    exec "a = 5"
    return [b for b in xrange(3)]
""", """
def f():
    exec "a = 5"
    return {b:b for b in xrange(3)}
""", """
def f():
    exec "a = 5"
    return {c for b in xrange(3)}
""", """
def f():
    exec "a = 5"
    return [c for b in xrange(3)]
""", """
def f():
    exec "a = 5"
    return {c:b for b in xrange(3)}
""", """
def f():
    global a
    def g():
        print a
        exec ""
""", """
def f():
    global a
    exec ""
    def g():
        print a
""", """
def f():
    global a
    def g():
        a = 0
        def h():
            exec ""
            print a
""", """
def f():
    a = 0
    def g():
        global a
        def h():
            exec ""
            print a
""", """
def f():
    a = 0
    def g():
        exec ""
        def h():
            print a
"""

]

#import traceback

for case in cases:
    print case
    try:
        exec case
    except SyntaxError as se:
        print se.message.replace("because ", "")
        # TODO uncomment this
        # traceback.print_exc()
