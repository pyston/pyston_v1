from __future__ import print_function
import _ast

# test compile string:
a = compile("'hello world'", "test.py", "single", _ast.PyCF_ONLY_AST)
exec compile(a, "test.py", "single")

compile('1 + 1', 'test.py', 'eval', dont_inherit=True)

# test compile ast:
def hello():
    print('hello again')

tree = compile('hello()', 'test.py', 'eval', _ast.PyCF_ONLY_AST)
exec compile(tree, '<tree>', 'eval', dont_inherit=True)

# test future flags:
exec compile('print(1, 2)', 'test.py', 'exec')

tree = compile('print(1, 2)', 'test.py', 'exec', _ast.PyCF_ONLY_AST)
exec compile(tree, '<tree>', 'exec')

c = compile('print(1, 2)', '', 'exec')
exec c
print(c.co_name, c.co_filename)

# test bad syntax which should not raise in compile time:
try:
    exec compile('break', '?', 'exec')
    assert False
except SyntaxError:
    pass

try:
    exec compile('continue', '?', 'exec')
    assert False
except SyntaxError:
    pass

# test duplicate args
try:
    exec compile('lambda x, x: none', 'foo', 'exec')
except SyntaxError as e:
    print(e)
else:
    raise Exception('SyntaxError not raised')

# test nests duplicate args
try:
    exec compile('lambda a, (b, (c, a)): none', 'foo', 'exec')
except SyntaxError as e:
    print(e)
else:
    raise Exception('SyntaxError not raised')

# test duplicate vararg and kwarg
try:
    eval('lambda *a, **a: 0')
except SyntaxError as e:
    print(e)
else:
    raise Exception('SyntaxError not raised')

try:
    eval('lambda a, *a: 0')
except SyntaxError as e:
    print(e)
else:
    raise Exception('SyntaxError not raised')
