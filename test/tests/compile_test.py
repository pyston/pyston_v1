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
