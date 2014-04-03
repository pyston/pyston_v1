import sys

class RangeLookup(object):
    def __init__(self):
        self.ranges = []

    def register(self, start, end, name):
        self.ranges.append((start, end, name))

    def lookup(self, addr):
        for start, end, name in self.ranges:
            if start <= addr < end:
                return "jit:" + name
        return hex(addr)

if __name__ == "__main__":
    raw_fn = sys.argv[1]
    jit_fns = sys.argv[2:]

    ranger = RangeLookup()

    for fn in jit_fns:
        for l in open(fn):
            start, end, name = l.split(' ', 2)
            name = name.strip()

            ranger.register(int(start, 16), int(end, 16), name)

    mode = 0
    for l in open(raw_fn):
        l = l.strip()
        if mode == 0:
            assert l == "--- symbol"
            mode = 1
            print l
        elif mode == 1:
            assert l == "binary=jit_pprof"
            mode = 2
            print l
        elif mode == 3:
            print l
        elif mode == 2:
            if l == "---":
                mode = 3
                print l
            else:
                addr, curname = l.split(' ', 1)
                if addr != curname:
                    print l
                else:
                    addr_int = int(addr, 16)
                    print addr, ranger.lookup(addr_int)
        else:
            assert 0, mode

