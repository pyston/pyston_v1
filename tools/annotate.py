import subprocess
import sys

def get_objdump(func):
    for l in open("perf_map/index.txt"):
        addr, this_func = l.split()
        if this_func == func:
            # print ' '.join(["objdump", "-b", "binary", "-m", "i386", "-D", "perf_map/" + func, "--adjust-vma=0x%s" % addr])
            p = subprocess.Popen(["objdump", "-b", "binary", "-m", "i386:x86-64", "-D", "perf_map/" + func, "--adjust-vma=0x%s" % addr], stdout=subprocess.PIPE)
            r = p.communicate()[0]
            assert p.wait() == 0
            return r

    raise Exception("Couldn't find function %r to objdump" % func)

if __name__ == "__main__":
    # TODO: if it's not passed, maybe default to annotating the
    # first function in the profile (the one in which the plurality of
    # the time is spent)?

    func = sys.argv[1]

    objdump =  get_objdump(func)

    p = subprocess.Popen(["perf", "annotate", "-v", func], stdout=subprocess.PIPE, stderr=open("/dev/null", "w"))
    annotate = p.communicate()[0]
    assert p.wait() == 0

    counts = {}
    for l in annotate.split('\n'):
        if ':' not in l:
            continue
        addr, count = l.split(':')
        addr = addr.strip()
        if addr == "h->sum":
            continue

        counts[addr] = int(count)

    for l in objdump.split('\n')[7:]:
        addr = l.split(':')[0]
        count = counts.pop(addr.strip(), 0)
        print str(count).rjust(8), l

    assert not counts, counts
