import _ast

def ast_parse(source, filename='<unknown>', mode='exec'):
    return compile(source, filename, mode, _ast.PyCF_ONLY_AST)


# TODO(kmod) unfortunately we don't expose any of the data members yet, and we don't even display
# the types correctly, so just parse the code and make sure that we didn't crash
ast_parse("print 1")
ast_parse("print 1", "t.py", "exec")
ast_parse("1", "t.py", "eval")

c = compile(ast_parse("print 1", "t.py", "exec"), "u.py", "exec")
print c.co_filename
exec c

try:
    c = compile(ast_parse("print 1", "t.py", "exec"), "u.py", "eval")
except Exception as e:
    print type(e)

c = compile(ast_parse("print 2", "t.py", "exec"), "u.py", "exec")
print eval(c)
