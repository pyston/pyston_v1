def print_when_eval(x, msg):
    print msg
    return x

# Test the order that decorators and function defaults get evaluated
@print_when_eval(lambda f: print_when_eval(lambda *args, **kw: print_when_eval(f, "calling function (outer)")(*args, **kw), "calling outer decorator"), "evaluating outer decorator")
@print_when_eval(lambda f: print_when_eval(lambda *args, **kw: print_when_eval(f, "calling function (inner)")(*args, **kw), "calling inner decorator"), "evaluating inner decorator")
def f(x=print_when_eval(1, "evaluating default")):
    return x
# Result: looks like it's decorators get evaluated first (outer in), then defaults, then the decorators are called (inside out).
print
print f()
print f(2)
print





def print_args(f):
    print "calling print_args decorator"
    def inner(*args, **kw):
        print args, kw
        return f(*args, **kw)

    return inner

def f1(a, b):
    print a + b

pf1 = print_args(f1)
pf1(1, 2)
pf1(2, b=2)

@print_args
def f2(a, b):
    print a - b

print f2.__name__
f2(1, 3)
f2(2, b=3)
