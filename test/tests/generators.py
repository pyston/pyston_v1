def G1(i=0):
    while True:
        yield i
        i += i

g1 = G1()
for i in range(5):
    print g1.next()
print g1.__name__



def G2():
    yield 1
    yield 2
    yield 3
g2a = G2()
g2b = G2()
print g2b.next()
print list(g2a)
print list(g2b)
print list(g2a)
print list(G2())



def G3(i=0):
    while True:
        got = (yield i**2)
        print "i=", i, "got=", got
        i += 1

g3 = G3();
g3.send(None)
for i in range(5):
    r = g3.send(i)
    print "received= ", r



def G4(i=1):
    1/0
    while True:
        print "unreachable"

try:
    print list(G4(0))
except ZeroDivisionError:
    print "catched a ZeroDivisionError"
    

def G5():
    i = 0
    try:
        while True:
            yield i
            i += 1
    except:
        print "catched a ZeroDivisionError inside G5"
    yield 42

g5 = G5()
for i in range(5):
    print g5.next()
print g5.throw(ZeroDivisionError)

def G6(a=[]):
    for i in range(2):
        a.append(i)
        yield a
print list(G6())
print list(G6())

def G7(p):
    a = p
    b = 2
    def G():
        yield a+b
    return G()
print list(G7(1))

def G8(*args):
    for a in args:
        yield a
print list(G8(1, 2, 3, 4, 5))

def G9(**kwargs):
    for a in sorted(kwargs.keys()):
        yield a, kwargs[a]
print list(G9(a="1", b="2", c="3", d="4", e="5"))

class MyStopIteration(StopIteration):
    pass
def G10():
    yield 1
    yield 2
    raise MyStopIteration, "test string"
print "list(G10()):", list(G10())
print "for i in G10():",
for i in G10():
    print i,
print

print "explicit t.next():"
g10 = G10()
g10.next()
g10.next()
try:
    g10.next()
except StopIteration as e:
    print "Caught exc1:", type(e), e
try:
    g10.next()
except StopIteration as e:
    print "Caught exc2:", type(e), e


def f():
    yield 1/0
g = f()

try:
    g.next()
except Exception as e:
    print type(e), e # ZeroDivisionError

try:
    g.next()
except Exception as e:
    print type(e), e # StopIteration


x = lambda: (yield 1)
print list(x())
x = lambda: ((yield 1), (yield 2))
print list(x())
