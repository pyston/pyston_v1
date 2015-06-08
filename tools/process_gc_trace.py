import os
import sys

if __name__ == "__main__":
    look_at = sys.argv[1]

    why = {}
    cur = None
    for l in open("gc_trace.txt"):
        if l.startswith("Pushing "):
            ptr = l.split()[1]
            why.setdefault(ptr, []).append(cur)
        else:
            cur = l

    def investigate(ptr):
        l = why.get(ptr)
        if not l:
            print "Not sure why %s is alive!" % ptr
        else:
            print ptr, "is alive from '%s'" % l[0].strip(),
            if len(l) > 1:
                print "and %d more entries" % (len(l) - 1)
            else:
                print
            if l[0].startswith("Looking at heap object"):
                prev_ptr = l[0].split()[-1]
                investigate(prev_ptr)

    investigate(look_at)
