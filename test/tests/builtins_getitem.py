# this file get's also included by the builtins.py to test that __builtins__ becomes a dict when getting imported
try:
    import types
    print type(__builtins__) == types.ModuleType
    __builtins__["all"]
    print "No exception"
except TypeError as e:
    print e
