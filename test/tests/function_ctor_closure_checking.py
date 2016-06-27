# expected: fail
# - we haven't implemented closure-checking (making sure when you set a closure that it is the right shape)

# Copied from https://github.com/networkx/networkx/blob/master/networkx/algorithms/isomorphism/matchhelpers.py
import types
def copyfunc(f, name=None):
    """Returns a deepcopy of a function."""
    try:
        # Python <3
        return types.FunctionType(f.func_code, f.func_globals,
                                  name or f.__name__, f.func_defaults,
                                  f.func_closure)
    except AttributeError:
        # Python >=3
        return types.FunctionType(f.__code__, f.__globals__,
                                  name or f.__name__, f.__defaults__,
                                  f.__closure__)

def f(x, y):
    def g(z=[]):
        z.append(x)
        z.append(y)
        print z
    return g
g1 = f(5, 6)

def f(x):
    def g(z=[]):
        z.append(x)
        print z
    return g
g2 = f(5)

def f(a, b):
    def g(z=[]):
        z.append(a)
        z.append(b)
        print z
    return g
g3 = f(5, 7)

c1 = types.FunctionType(g1.func_code, g1.func_globals, g1.func_name, g1.func_defaults, g1.func_closure)
c1()
g1()

try:
    # No closure:
    types.FunctionType(g1.func_code, g1.func_globals, g1.func_name, g1.func_defaults, None)
    assert 0
except Exception as e:
    print e

try:
    # Wrong closure:
    types.FunctionType(g1.func_code, g1.func_globals, g1.func_name, g1.func_defaults, g2.func_closure)
    assert 0
except Exception as e:
    print e

# Different names are fine since they get erased from the closure:
c3 = types.FunctionType(g1.func_code, g1.func_globals, g1.func_name, g1.func_defaults, g3.func_closure)
c3()

