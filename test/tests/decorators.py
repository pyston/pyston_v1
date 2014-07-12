# expected: fail
# - decorators

def print_when_eval(x, msg):
    print msg
    return x

# Test the order that decorators and function defaults get evaluated
@print_when_eval(lambda f: print_when_eval(f, "calling decorator"), "evaluating decorator")
def f(x=print_when_eval(1, "evaluating default")):
    pass
# Result: looks like it's decorator first, then defaults, then the decorator is called.
