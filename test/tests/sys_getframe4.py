# Make sure we can throw exceptions through frame we called _getframe

import sys

def g():
    sys._getframe(1)
    1/0

def f():
    g()

try:
    f()
except Exception as e:
    print e
