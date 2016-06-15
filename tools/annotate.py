import argparse
import commands
import os
import re
import subprocess
import sys

def get_objdump(func):
    for l in open(args.perf_map_dir + "/index.txt"):
        addr, this_func = l.split()
        if this_func == func:
            obj_args = ["objdump", "-b", "binary", "-m", "i386:x86-64", "-D", args.perf_map_dir + "/" + func, "--adjust-vma=0x%s" % addr]
            if not args.print_raw_bytes:
                obj_args += ["--no-show-raw"]
            p = subprocess.Popen(obj_args, stdout=subprocess.PIPE)
            r = p.communicate()[0]
            assert p.wait() == 0
            return r
    raise Exception("Couldn't find function %r to objdump" % func)

def getNameForAddr(addr):
    for l in open(args.perf_map_dir + "/index.txt"):
        this_addr, this_func = l.split()
        if int(this_addr, 16) == addr:
            return this_func
    raise Exception("Couldn't find function with addr %x" % addr)

_symbols = None

def demangle(sym):
    if os.path.exists("tools/demangle"):
        demangled = commands.getoutput("tools/demangle %s" % sym)
        if demangled == "Error: unable to demangle":
            demangled = sym
    else:
        demangled = commands.getoutput("c++filt %s" % sym)
    return demangled

def lookupAsSymbol(n):
    global _symbols
    if _symbols is None:
        _symbols = {}
        nm_output = commands.getoutput("nm pyston_release")
        for l in nm_output.split('\n'):
            addr = l[:16]
            if addr.isalnum():
                _symbols[int(addr, 16)] = l[19:]
    sym = _symbols.get(n, None)
    if not sym:
        return sym

    if sym.startswith('_'):
        demangled = demangle(sym)
        # perf report does not like '<'
        return demangled.replace("<", "_")
    return sym

_heap_proc = None
heapmap_args = None
def lookupAsHeapAddr(n):
    global _heap_proc
    if _heap_proc is None:
        if heapmap_args is None:
            return None

        _heap_proc = subprocess.Popen(heapmap_args, stdout=subprocess.PIPE, stdin=subprocess.PIPE)

        while True:
            l = _heap_proc.stdout.readline()
            if l.startswith("Pyston v"):
                break

    _heap_proc.stdin.write("dumpAddr(%d)\nprint '!!!!'\n" % n)
    lines = []
    while True:
        l = _heap_proc.stdout.readline()
        assert l, "heapmap subprocess exited? code: %r" % _heap_proc.poll()
        if l == '!!!!\n':
            break
        lines.append(l)
    s = ''.join(lines[1:-1])
    if "Class: NoneType" in s:
        return "None"
    if "non-gc memory" in s:
        return "(non-gc memory)"
    if "Hidden class object" in s:
        return "(hcls object)"
    if "Class: type" in s:
        m = re.search("Type name: ([^ \n]+)", s)
        return "The '%s' class" % m.group(1)

    if "Python object" in s:
        m = re.search("Class: ([^ \n]+)", s)
        return "A '%s' object" % m.group(1)

    print s

def lookupConstant(n):
    sym = lookupAsSymbol(n)
    if sym:
        return "; " + sym

    heap = lookupAsHeapAddr(n)
    if heap:
        return "; " + heap

    return ""

def getCommentForInst(inst):
    patterns = ["movabs \\$0x([0-9a-f]+),",
                "mov    \\$0x([0-9a-f]+),",
                "cmpq   \\$0x([0-9a-f]+),",
                "callq  0x([0-9a-f]+)",
                ]

    for pattern in patterns:
        m = re.search(pattern, inst)
        if m:
            n = int(m.group(1), 16)
            if n:
                return lookupConstant(n)
    return None

def printLine(count, inst, extra = ""):
    if args.print_perf_counts:
        print str(count).rjust(8),
    print inst.ljust(70), extra

if __name__ == "__main__":
    # TODO: if it's not passed, maybe default to annotating the
    # first function in the profile (the one in which the plurality of
    # the time is spent)?

    parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("func_name", metavar="FUNC_NAME_OR_ADDR", help="name or address of function to inspect")
    parser.add_argument("--collapse-nops", dest="collapse_nops", action="store", default=5, type=int)
    parser.add_argument("--no-collapse-nops", dest="collapse_nops", action="store_false")
    parser.add_argument("--heap-map-args", nargs='+', help="""
Command to run that will provide heap map information.
This will typically look like:
--heap-map-args ./pyston_release -i BENCHMARK
    """.strip())
    parser.add_argument("--heap-map-target", help="""
Benchmark that was run.  '--heap-map-target BENCHMARK' is
equivalent to '--heap-map-args ./pyston_release -i BENCHMARK'.
    """.strip())
    parser.add_argument("--perf-data", default="perf.data")
    parser.add_argument("--perf-map-dir", default="perf_map")
    parser.add_argument("--print-raw-bytes", default=True, action='store_true')
    parser.add_argument("--no-print-raw-bytes", dest="print_raw_bytes", action='store_false')
    parser.add_argument("--print-perf-counts", default=True, action='store_true')
    parser.add_argument("--no-print-perf-counts", dest="print_perf_counts", action='store_false')
    args = parser.parse_args()

    if args.func_name.lower().startswith("0x"):
        addr = int(args.func_name, 16)
        func = getNameForAddr(addr)
    else:
        func = args.func_name

    if args.heap_map_args:
        heapmap_args = args.heap_map_args
    elif args.heap_map_target:
        heapmap_args = ["./pyston_release", "-i", args.heap_map_target]

    objdump = get_objdump(func)

    p = subprocess.Popen(["perf", "annotate", "-i", args.perf_data, "-v", func], stdout=subprocess.PIPE, stderr=open("/dev/null", "w"))
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

    nop_lines = [] # list of tuple(int(count), str(line))
    for l in objdump.split('\n')[7:]:
        addr = l.split(':')[0]
        count = counts.pop(addr.strip(), 0)
        extra = getCommentForInst(l) or ""

        if args.collapse_nops and l.endswith("\tnop"):
            nop_lines.append((count, l))
        else:
            if len(nop_lines):
                if len(nop_lines) <= args.collapse_nops:
                    for nop in nop_lines:
                        printLine(nop[0], nop[1])
                else:
                    sum_count = sum([nop[0] for nop in nop_lines])
                    addr_start = int(nop_lines[0][1].split(':')[0], 16)
                    addr_end = int(nop_lines[-1][1].split(':')[0], 16)
                    addr_range = ("%x-%x" % (addr_start, addr_end)).ljust(29)
                    printLine(sum_count, "    %s              nop*%d" % (addr_range, len(nop_lines)))
                nop_lines = []
            printLine(count, l, extra)

    assert not counts, counts
