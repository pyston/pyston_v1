# Simple "active LOC" analyzer
# It measures which lines get executed and how long they take to execute,
# and calculates the number of lines that represent 99% (configurable) of the total runtime.
#
# this kind of "works" but ends up saying that most lines do a constant amount of work, since the tracer work dominates
# - ie it will say that a simple line "None" will end up being just as expensive as any other
# Also, I think the location needs to be updated on "call" and "return" events

import os.path
import runpy
import sys
import time

class Tracer(object):
    def __init__(self):
        self.times = {}
        self.start = None
        self.cur = None

    def log(self, loc):
        start = self.start
        now = self.now
        if start is None:
            return

        cur = self.cur
        self.times[cur] = self.times.get(cur, 0) + (now - start) - 0.000000
        self.cur = loc

    def trace(self, frame, event, arg):
        self.now = time.time()
        if event == "line":
            loc = frame.f_code.co_filename, frame.f_lineno
            # print loc
            self.log(loc)
            self.start = time.time()

        return self.trace

if __name__ == "__main__":
    old_sys_argv = sys.argv
    fn = sys.argv[1]
    args = sys.argv[2:]
    sys.argv = [sys.argv[0]] + args

    t = Tracer()

    sys.settrace(t.trace)

    assert sys.path[0] == os.path.abspath(os.path.dirname(__file__))
    sys.path[0] = os.path.abspath(os.path.dirname(fn))

    # del sys.modules["__main__"] # do we need this?
    runpy.run_path(fn, run_name="__main__")
    sys.settrace(None)


    times = t.times.items()
    times.sort(key=lambda (l, t): t, reverse=True)

    total = 0.0
    for l, t in times:
        total += t
    print total, len(times)

    FRACTION = 0.99

    sofar = 0.0
    total_lines = 0
    for (l, t) in times:
        if not l:
            continue
        fn, lineno = l
        print "%s:%s  %f" % (fn, lineno, t)
        total_lines += 1
        sofar += t
        if sofar >= total * FRACTION:
            break
    print total_lines
