def wrapper(f):
    return f

@wrapper
@wrapper
def foo():
    pass

# Should print the line that the first "@wrapper" is on:
print foo.func_code.co_firstlineno

import inspect
print repr(inspect.getsource(foo))
