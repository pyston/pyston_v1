f1 = lambda: globals()['__name__']
f2 = lambda: __name__

print f1()
print f2()

import import_target
print import_target.letMeCallThatForYou(f1)
print import_target.letMeCallThatForYou(f2)
print import_target.letMeCallThatForYou(globals)['__name__']

try:
    print x
    assert 0, "Expected NameError not thrown"
except NameError:
    pass

# You're allowed to assign through globals and have it affect the module:
globals()['x'] = 1
print x

# locals should do the same as globals
print locals()['x']
locals()['x'] = 2
print x
