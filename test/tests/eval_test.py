# expected: fail
# - eval not implemented
# - closures not implemented

print eval("3 + 4")

a = 5
print eval("a")

print eval("[b for b in range(5)]")
print b

c = 2
print eval("[c for c in range(5)]")
print c

try:
    print eval("int('abc')")
except ValueError:
    print 'got ValueError'

d = 19
e = 20
i = 21
def func():
    print eval("d")

    e = 20
    print eval("e")

    eval("[f for f in range(5)]")

    eval("[g for g in range(5)]")
    try:
        g
    except NameError:
        print 'g not found'

    h = 2
    eval("[h for h in range(5)]")
    print h

    eval("[i for i in range(5)]")

    j = 24
    def inner():
        return j
    print eval("inner()")
func()
print i

print eval("(lambda k : k+2)(3)")

l = 100
print eval("(lambda k : l+2)(3)")

print eval("(lambda k : [m for m in range(5)])(3)")
try:
    print m
except NameError:
    print 'm not found'

n = 200
print eval("(lambda k : [n for n in range(5)])(3)")
print n

print eval("eval('3 + 2342')")

x = 2
def wrap():
    x = 1
    y = 3

    # The eval('x') in this function will resolve to the global scope:
    def inner1():
        y
        print locals()
        print eval('x')
    inner1()

    # The eval('x') in this function will resolve to the closure, since
    # there is a textual reference to x which causes it to get captured:
    def inner2():
        x
        print locals()
        print eval('x')
    inner2()

wrap()
