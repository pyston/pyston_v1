# run_args: -n
# statcheck: noninit_count('slowpath_getattr') <= 10
# statcheck: noninit_count('slowpath_setattr') <= 10

class C(object):
    pass

def f(obj, name):
    obj.__name__ = name
    print obj.__name__

# pass in a class each time
for i in xrange(1000):
    f(C, str(i))

# TODO test guards failing
# I think we need to get a getset descriptor that isn't __name__
# or implement __dict__ to write such a test: otherwise it's impossible
# to actually get our hands on the descriptor object :\
