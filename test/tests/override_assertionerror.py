
orig_ae = AssertionError
class MyAssertionError(Exception):
    pass

s = """
try:
    assert 0
except Exception as e:
    print type(e)
"""

exec s

import __builtin__
__builtin__.AssertionError = MyAssertionError
exec s

class MyAssertionError2(Exception):
    pass
AssertionError = MyAssertionError2
exec s
exec s in {}

class MyAssertionError3(Exception):
    pass

def f1():
    # assert is hardcoded to look up "AssertionError" in the global scope,
    # even if there is a store to it locally:
    AssertionError = MyAssertionError3
    exec s

    try:
        assert 0
    except Exception as e:
        print type(e)
f1()
