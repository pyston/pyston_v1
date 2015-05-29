import sys

if __name__ == "__main__":
    fn = sys.argv[1]

    tracebacks = []
    cur_traceback = []
    with open(fn) as f:
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
        locations = [l for l in t.split('\n') if l.startswith("  File")]

        last_file = locations[-1].split('"')[1]
        last_function = locations[-1].split()[-1][:-1]

        # dedupe on:
        # key = t # full traceback
        # key = '\n'.join(t.split('\n')[-8:]) # last 4 stack frames
        # key = '\n'.join(t.split('\n')[-4:]) # last 2 stack frames
        # key = '\n'.join(t.split('\n')[-2:]) # last stack frame
        # key = last_file, last_function
        key = last_file
        counts[key] = counts.get(key, 0) + 1

    n = len(tracebacks)

    NUM_DISPLAY = 6

    entries = sorted(counts.items(), key=lambda (k, v): v)

    if len(counts) > NUM_DISPLAY:
        num_hidden = 0
        counts_hidden = 0

        for k, v in entries[:-NUM_DISPLAY]:
            num_hidden += 1
            counts_hidden += v
        print "Hiding %d entries that occurred %d (%.1f%%) times" % (num_hidden, counts_hidden, 100.0 * counts_hidden / n)

    for k, v in sorted(counts.items(), key=lambda (k, v): v)[-NUM_DISPLAY:]:
        print
        print "Occurs %d (%.1f%%) times:" % (v, 100.0 * v / n)
        print k

    print
    print "Total tracebacks:", n
