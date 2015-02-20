# lots of eval functionality not implemented

print eval("3 + 4")

a = 5
print eval("a")

#print eval("[b for b in range(5)]")
#print b

#c = 2
#print eval("[c for c in range(5)]")
#print c

#try:
#    print eval("int('abc')")
#except ValueError:
#    print 'got ValueError'

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

    eval("[g2 for g2 in range(5)]")
    try:
        print g2
    except NameError:
        print 'g2 not found'
    g2 = 5

    h = 2
    eval("[h for h in range(5)]")
    print h

#    h2 = 2
#    print eval("h2 + sum([h2 for h2 in range(5)])")
#    print 'h2', h2

#    h3 = 2
#    print eval("sum([h3 for h3 in range(5)]) + h3")
#    print 'h3', h3

    eval("[i for i in range(5)]")

    #j = 24
    #def inner():
    #    return j
    #print eval("inner()")
func()
print i

#print eval("(lambda k : k+2)(3)")

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
o = 300
print 'eval eval o', eval("eval('o')")

#print eval("[(lambda p : p + o)(5) for o in range(5)]")

"""
shadow1 = 1000
shadow2 = 1000
shadow3 = 1000
def func2():
    shadow1 = 2000
    print 'shadow1', eval("shadow1")

    shadow2 = 2000
    eval("[shadow2 for shadow2 in range(5)]")
    print 'shadow2', shadow2

    print 'shadow3', eval("shadow3 + sum([2 for shadow3 in range(5)]) + shadow3")
func2()
print 'shadow1', shadow2
print 'shadow2', shadow2
print 'shadow3', shadow3

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
"""
