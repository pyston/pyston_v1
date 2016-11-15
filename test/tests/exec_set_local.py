a = 0
b = 0

def bare_exec_func():
    a = 1
    exec "a = 5; b = 5"
    # NAME lookup:
    print a
    print locals()['a']
    # NAME lookup:
    print b
bare_exec_func()
print

def nonbare_exec_func():
    a = 1
    exec "a = 5; b = 5" in {}, locals()
    # FAST lookup:
    print a
    print locals()['a']
    # NAME lookup:
    print b
nonbare_exec_func()
print

try:
    exec """
def bare_exec_nested():
    a = 1
    exec "a = 5; b = 5"
    def g():
        print a
        print b
    """
    assert 0
except SyntaxError:
    pass
print

def nonbare_exec_nested():
    a = 1
    exec "a = 5; b = 5" in {}, locals()
    def g():
        # DEREF
        print a
        # GLOBAL
        print b
    g()
    # CLOSURE
    print a
    # NAME
    print b
    print locals()['a']
nonbare_exec_nested()
print

class C(object):
    a = 1
    exec "a = 5; b = 5" in {}, locals()

    def g():
        # GLOBAL:
        print a
        # GLOBAL:
        print b
    g()

    # NAME:
    print a
    # NAME:
    print b
print
