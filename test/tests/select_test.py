import select

for k in sorted(dir(select)):
    if not k.startswith("EPOLL") and not k.startswith("POLL"):
        continue

    if k == "EPOLLET":
        # On 32-bit versions of CPython, select.EPOLLET (==1<<31) overflows a 32-bit signed integer
        # and becomes INT_MIN.  But really, it's a bitmask so it's an unsigned value and imho should
        # be provided as a positive number.
        # Since we only build Pyston in 64-bit mode, EPOLLET is always positive in Pyston.  If we
        # see that it's negative, just make sure that it probably hit this behavior (wrapped around)
        v = select.EPOLLET
        if v < 0:
            import sys
            assert (-v) > sys.maxint
        print abs(v)
    else:
        print k, getattr(select, k)

