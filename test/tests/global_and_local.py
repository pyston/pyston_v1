# I would have expected this to be valid, but cPython and pypy err out saying "name 'x' is local and global"
# Interestingly, this SyntaxError gets thrown *after* AST creation, which I guess makes sense but I wasn't
# expecting

x = 1
def f(x):
    global x

print "calling"
f(2)
print x
