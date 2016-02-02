# this is copied out of cpythons test_sys.py and adopted to use assert stmts
import sys
import thread
import threading, thread
import traceback

# Spawn a thread that blocks at a known place.  Then the main
# thread does sys._current_frames(), and verifies that the frames
# returned make sense.
entered_g = threading.Event()
leave_g = threading.Event()
thread_info = []  # the thread's id

def f123():
    g456()

def g456():
    thread_info.append(thread.get_ident())
    entered_g.set()
    leave_g.wait()

t = threading.Thread(target=f123)
t.start()
entered_g.wait()

# At this point, t has finished its entered_g.set(), although it's
# impossible to guess whether it's still on that line or has moved on
# to its leave_g.wait().
assert len(thread_info) == 1
thread_id = thread_info[0]

d = sys._current_frames()

main_id = thread.get_ident()
assert main_id in d
assert thread_id in d

# Verify that the captured main-thread frame is _this_ frame.
frame = d.pop(main_id)
assert frame is sys._getframe()

# Verify that the captured thread frame is blocked in g456, called
# from f123.  This is a litte tricky, since various bits of
# threading.py are also in the thread's call stack.
frame = d.pop(thread_id)
stack = traceback.extract_stack(frame)
for i, (filename, lineno, funcname, sourceline) in enumerate(stack):
    if funcname == "f123":
	break
else:
    self.fail("didn't find f123() on thread's call stack")

assert sourceline == "g456()"

# And the next record must be for g456().
filename, lineno, funcname, sourceline = stack[i+1]
assert funcname == "g456"
assert sourceline in ["leave_g.wait()", "entered_g.set()"]

# Reap the spawned thread.
leave_g.set()
t.join()

print "finished"
