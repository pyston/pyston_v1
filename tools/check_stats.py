import argparse
import os
import subprocess
import sys
import time

if __name__ == "__main__":
    cmd = ["./pyston_release", "-Ts", sys.argv[1]]

    start = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out, err = p.communicate()
    assert p.wait() == 0
    elapsed = time.time() - start
    open('tmp.txt', 'w').write(out)

    # elapsed = 1.5
    # out = open("tmp.txt").read()

    stats = []
    is_counter = False
    for l in out.split('\n'):
        if l.strip() == "Counters:":
            is_counter = True
            continue
        if l.strip() == "(End of stats)":
            is_counter = False
            continue

        if not is_counter:
            continue

        name, count = l.split(':')
        count = int(count)
        if name.startswith('us_timer'):
            stats.append((name, 0.000001 * count))
        # if name.startswith('slowpath'):
            # stats.append((name, 0.0000001 * count))

    stats.sort(key=lambda (name,s): s, reverse=True)
    print "Most interesting stats:"
    for (name, s) in stats[:10]:
        print "% 40s %.3fs (%.0f%%)" % (name, s, 100.0 * s / elapsed)
    print "%.1fs total time" % elapsed
