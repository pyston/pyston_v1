"""
Simple "active LOC" analyzer

Runs a program, and using a sampling profiler, outputs some statistics about how many
lines of code contribute to the majority of the runtime.

For example:
$ python tools/measure_loc.py minibenchmarks/fannkuch_med.py
[...]
Found 36 unique lines with 116480 samples
minibenchmarks/fannkuch_med.py:28                   48244 41.4%   1 41.4%
minibenchmarks/fannkuch_med.py:36                   18703 16.1%   2 57.5%
minibenchmarks/fannkuch_med.py:30                   8835  7.6%   3 65.1%
minibenchmarks/fannkuch_med.py:37                   6388  5.5%   4 70.5%
minibenchmarks/fannkuch_med.py:29                   5348  4.6%   5 75.1%
minibenchmarks/fannkuch_med.py:27                   4562  3.9%   6 79.1%
minibenchmarks/fannkuch_med.py:20                   3599  3.1%   7 82.1%
minibenchmarks/fannkuch_med.py:21                   2985  2.6%   8 84.7%
minibenchmarks/fannkuch_med.py:26                   2984  2.6%   9 87.3%
minibenchmarks/fannkuch_med.py:23                   2835  2.4%  10 89.7%
minibenchmarks/fannkuch_med.py:24                   2781  2.4%  11 92.1%
minibenchmarks/fannkuch_med.py:40                   2089  1.8%  12 93.9%
minibenchmarks/fannkuch_med.py:38                   2038  1.7%  13 95.6%
minibenchmarks/fannkuch_med.py:35                   1990  1.7%  14 97.3%
minibenchmarks/fannkuch_med.py:19                   1769  1.5%  15 98.9%
minibenchmarks/fannkuch_med.py:39                   1108  1.0%  16 99.8%
minibenchmarks/fannkuch_med.py:42                   179  0.2%  17 100.0%
minibenchmarks/fannkuch_med.py:49                   10  0.0%  18 100.0%
minibenchmarks/fannkuch_med.py:51                    7  0.0%  19 100.0%
/usr/lib/python2.7/runpy.py:220                      3  0.0%  20 100.0%
(and 16 more -- see measure_loc.pkl)
Picked 2 lines out of 36 to reach 57.48%
Picked 5 lines out of 36 to reach 75.14%
Picked 11 lines out of 36 to reach 92.09%
Picked 16 lines out of 36 to reach 99.81%


By default, this tool reports lines of code by the amount of time that was spent on them.
There is also a mode to change the accounting to "number of times the line executed";
use the python_trace_counter instead of python_sampler (you have to modify the script).
"""


import cPickle
import os
import runpy
import signal
import sys
import time
import traceback

class SamplingProfiler(object):
    # Copied + modified from https://github.com/bdarnell/plop/blob/master/plop/collector.py
    MODES = {
        'prof': (signal.ITIMER_PROF, signal.SIGPROF),
        'virtual': (signal.ITIMER_VIRTUAL, signal.SIGVTALRM),
        'real': (signal.ITIMER_REAL, signal.SIGALRM),
        }

    def __init__(self, sighandler, dumper, mode, interval=0.0001):
        self.sighandler = sighandler
        self.dumper = dumper
        self.mode = mode
        self.interval = interval

    def start(self):
        timer, sig = SamplingProfiler.MODES[self.mode]

        signal.signal(sig, signal_handler)
        signal.setitimer(timer, self.interval, self.interval)

    def stop(self):
        timer, sig = SamplingProfiler.MODES[self.mode]
        signal.setitimer(timer, 0, 0)
        signal.signal(sig, signal.SIG_DFL)
        return self.dumper()

# Try to prevent / notice if someone else sets a debugger.
# (Note: removing sys.settrace is not sufficient since one can set
# frame.f_trace)
sys_settrace = sys.settrace
sys.settrace = None
import bdb
bdb.Bdb.set_trace = None
bdb.set_trace = None
import pdb
pdb.set_trace = None
pdb.Pdb.set_trace = None

class TracingProfiler(object):
    def __init__(self, tracefunc, dumper):
        self.tracefunc = tracefunc
        self.dumper = dumper

    def start(self):
        sys_settrace(self.tracefunc)

    def stop(self):
        assert sys.gettrace() == self.tracefunc, "Problem!  Someone/something removed our tracer.  It's now: %r" % sys.gettrace()
        sys_settrace(None)
        return self.dumper()

times = {}
start_time = time.time()
SKIP_WARMUP = 0
def signal_handler(sig, frame):
    if time.time() >= start_time + SKIP_WARMUP:
        print "Starting sampling"
        def real_signal_handler(sig, frame):
            loc = frame.f_code.co_filename, frame.f_lineno
            times[loc] = times.get(loc, 0) + 1

        signal.signal(sig, real_signal_handler)
        real_signal_handler(sig, frame)
    return

def trace_count(frame, event, arg):
    if event == "line":
        loc = frame.f_code.co_filename, frame.f_lineno
        times[loc] = times.get(loc, 0) + 1

    return trace_count

def get_times():
    return times.items()

def run(sampler, kind):
    fn = sys.argv[1]

    if fn == '-m':
        module = sys.argv[2]
        args = sys.argv[3:]
    else:
        args = sys.argv[2:]
    sys.argv = [sys.argv[0]] + args

    sys.path[0] = os.path.abspath(os.path.dirname(fn))

    sampler.start()

    # del sys.modules["__main__"] # do we need this?
    try:
        if fn == '-m':
            runpy.run_module(module, run_name="__main__")
        else:
            runpy.run_path(fn, run_name="__main__")
    except KeyboardInterrupt:
        print "Interrupted!"
        traceback.print_exc()
    except SystemExit:
        pass
    except:
        print "ERROR!"
        traceback.print_exc()

    print "Stopping timer and tallying statistics..."
    times = sampler.stop()

    times.sort(key=lambda (l, t): t, reverse=True)
    with open("measure_loc.pkl", "w") as f:
        cPickle.dump(times, f)

    total = 0.0
    for l, t in times:
        total += t
    if kind == "time":
        print "Found %d unique lines for a total of %.2fs" % (len(times), total)
    else:
        print "Found %d unique lines with %d samples" % (len(times), total)

    FRACTIONS = [0.5, 0.75, 0.9, 0.99, 1]
    frac_counts = []
    frac_fracs = []
    frac_idx = 0
    DISPLAY_THRESH = 20

    sofar = 0.0
    total_lines = 0
    for (l, t) in times:
        if not l:
            continue
        fn, lineno = l
        total_lines += 1
        sofar += t
        if total_lines <= DISPLAY_THRESH:
            if kind == "time":
                print ("%s:%s" % (fn, lineno)).ljust(50), "%.4fs %4.1f%% % 3d %4.1f%%" % (t, t / total * 100, total_lines, sofar / total * 100.0)
            else:
                print ("%s:%s" % (fn, lineno)).ljust(50), "% 3d %4.1f%% % 3d %4.1f%%" % (t, t / total * 100, total_lines, sofar / total * 100.0)
        if sofar >= total * FRACTIONS[frac_idx]:
            if FRACTIONS[frac_idx] == 1:
                break

            frac_counts.append(total_lines)
            frac_fracs.append(sofar)
            frac_idx += 1

    if len(times) > DISPLAY_THRESH:
        print "(and %d more -- see measure_loc.pkl)" % (len(times) - DISPLAY_THRESH)

    assert len(frac_counts) == len(FRACTIONS) -1
    for i in xrange(len(frac_counts)):
        print "Picked %d lines out of %d to reach %.2f%%" % (frac_counts[i], len(times), frac_fracs[i] / total * 100.0)

python_sampler = SamplingProfiler(signal_handler, get_times, "real", interval=0.0001)
python_trace_counter = TracingProfiler(trace_count, get_times)
try:
    import measure_loc_ext
    cext_trace_timer = TracingProfiler(measure_loc_ext.trace, lambda: measure_loc_ext.get_times().items())
except ImportError:
    print "(extension module not available)"

if __name__ == "__main__":
    if sys.argv[1] == '-t':
        del sys.argv[1]
        run(cext_trace_timer, "time")
    else:
        run(python_sampler, "count")
    # run(python_trace_counter, "count")
