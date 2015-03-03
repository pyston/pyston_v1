# should_error

"docstring"

def f():
    class C(object):
        from __future__ import division # should cause syntax error
