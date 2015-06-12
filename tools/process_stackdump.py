import argparse
import sys

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("tracebacks_file", action="store", default=None)
    parser.add_argument("--num-display", action="store", default=6, type=int)
    parser.add_argument("--dedup-frames", action="store", default=None, type=int)
    parser.add_argument("--dedup-file", action="store", const=1, nargs='?')
    parser.add_argument("--dedup-function", action="store_true")
    args = parser.parse_args()

    num_display = args.num_display

    key_func = None
    def traceback_locations(t):
        return [l for l in t.split('\n') if l.startswith("  File")]
    if args.dedup_frames:
        assert not key_func
        key_func = lambda t: '\n'.join(t.split('\n')[-2 * args.dedup_frames:]) # last 4 stack frames
    if args.dedup_file:
        assert not key_func
        def key_func(t):
            locs = traceback_locations(t)
            prev_f = None
            n = int(args.dedup_file)
            files = []
            for l in reversed(locs):
                f = l.split('"')[1]
                if f == prev_f:
                    continue
                prev_f = f
                files.append("  " + f)
                if len(files) == n:
                    break
            return '\n'.join(reversed(files))
    if args.dedup_function:
        assert not key_func
        def key_func(t):
            locations = traceback_locations(t)
            last_file = locations[-1].split('"')[1]
            last_function = locations[-1].split()[-1][:-1]
            return "%s::%s()" % (last_file, last_function)
    if not key_func:
        key_func = lambda t: t

    tracebacks = []
    cur_traceback = []
    with open(args.tracebacks_file) as f:
        for l in f:
            if l.startswith("Traceback"):
                if cur_traceback:
                    tracebacks.append(''.join(cur_traceback).rstrip())
                cur_traceback = []
            elif not (l.startswith("  File") or l.startswith("    ")):
                print "non-traceback line?  ", l.strip()
                continue
            else:
                cur_traceback.append(l)
    if cur_traceback:
        tracebacks.append(''.join(cur_traceback).rstrip())
        cur_traceback = []

    counts = {}
    for t in tracebacks:
        key = key_func(t)
        counts[key] = counts.get(key, 0) + 1

    n = len(tracebacks)


    entries = sorted(counts.items(), key=lambda (k, v): v)

    if len(counts) > num_display:
        num_hidden = 0
        counts_hidden = 0

        for k, v in entries[:-num_display]:
            num_hidden += 1
            counts_hidden += v
        print "Hiding %d entries that occurred %d (%.1f%%) times" % (num_hidden, counts_hidden, 100.0 * counts_hidden / n)

    for k, v in sorted(counts.items(), key=lambda (k, v): v)[-num_display:]:
        print
        print "Occurs %d (%.1f%%) times:" % (v, 100.0 * v / n)
        print k

    print
    print "Total tracebacks:", n
