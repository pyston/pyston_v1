
X = 0
Y = 0
def wrapper():
    X = 1
    Y = 1
    class C(object):
        global Y
        X = 2
        Y = 2
        def f(self):
            # These references should skip all of the classdef directives,
            # and hit the definitions in the wrapper() function
            print X
            print Y
    return C

wrapper()().f()

print X
print Y # got changed in classdef for C
