# allow-warning: import level 0 will be treated as -1
# skip-if: '-x' in EXTRA_JIT_ARGS

def f(a):
    print a

try:
    f(**{u'a\u0180':3})
except TypeError as e:
    print e

try:
    setattr(object(), u"\u0180", None)
except TypeError as e:
    print e

try:
    hasattr(object(), u"\u0180")
except TypeError as e:
    print e

