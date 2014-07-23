# expected: fail
# - decorators

def f(o, msg):
    print msg
    return o

@f(lambda c: f(c, "calling decorator"), "evaluating decorator object")
class C(f(object, "evaluating base")):
    print "in classdef"
