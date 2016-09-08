# I would have expected this to be valid, but cPython and pypy err out saying "name 'x' is local and global"
try:
    exec """
x = 1
def f(x):
    global x

print "calling"
f(2)
print x
"""
except SyntaxError as e:
    print e.message

try:
    exec """
x = 1
def f((x, y)):
    global x

print "calling"
f(2)
print x
"""
except SyntaxError as e:
    print e.message

try:
    exec """
def f(*args):
    global args

print "calling"
f(2)
print x
"""
except SyntaxError as e:
    print e.message

try:
    exec """
def f(**kwargs):
    global kwargs

print "calling"
f(d=2)
print x
"""
except SyntaxError as e:
    print e.message

try:
    exec """
class C(object):
    def f(__a):
        global __a

print "calling"
f(d=2)
print x
"""
except SyntaxError as e:
    print e.message
