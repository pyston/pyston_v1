# Some cases that are tricky to get f_lineno right.
# (It's not super critical to get them right in all these cases, since
# in some of them it only shows up if you inspect the frame after-exit
# which seems not very common.  But we should at least try to return
# something reasonable [non-zero]).

import sys

fr = None
def f():
    global fr
    fr = sys._getframe(0)

f()
print fr.f_lineno

s = """
import sys
fr = sys._getframe(0)
""".strip()

exec s
print fr.f_lineno


def f2():
    try:
        1/1
        1/0
        1/1
    except ImportError:
        assert 0
        pass
    except ValueError:
        assert 0
        def f():
            pass
try:
    f2()
    assert 0
except:
    # These will be different!
    print sys.exc_info()[2].tb_next.tb_frame.f_lineno
    print sys.exc_info()[2].tb_next.tb_lineno

def f5():
    print "f5"
    try:
        1/0
    finally:
        pass
try:
    f5()
    assert 0
except:
    # These will be different!
    print sys.exc_info()[2].tb_next.tb_frame.f_lineno
    print sys.exc_info()[2].tb_next.tb_lineno

def f6():
    print "f6"
    with open("/dev/null"):
        1/0
        1/1
try:
    f6()
    assert 0
except:
    # These will be different!
    print sys.exc_info()[2].tb_next.tb_frame.f_lineno
    print sys.exc_info()[2].tb_next.tb_lineno

def f3(n):
    global fr
    fr = sys._getframe(0)
    try:
        1/n
    except ZeroDivisionError:
        pass
    except:
        pass

f3(1)
print fr.f_lineno
f3(0)
print fr.f_lineno

def f4():
    global fr
    fr = sys._getframe(0)
    yield 1
    yield 2
    yield 3
g = f4()
print g.next()
print fr.f_lineno

fr_l = []
g = (i for i in [fr_l.append(sys._getframe(0)), xrange(10)][1])
print g.next()
print fr_l[0].f_lineno

print repr(eval(u"\n\n__import__('sys')._getframe(0).f_lineno"))
