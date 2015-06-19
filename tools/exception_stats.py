import argparse
import os
import subprocess
import sys
import time

def output_stats(stats, total_count):
    if total_count is 0:
        return
    stats.sort(key=lambda (name,s): s, reverse=True)
    for (name, s) in stats[:5]:
        print "%80s  %d (%.0f%%)" % (name, s, 100.0 * s / total_count)

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

    is_counter = False

    current_header = ""
    current_count = 0
    stats = []
    
    for l in out.split('\n'):
        if not l.startswith('num_exceptions_'):
            continue

        if '<' in l:
            name = l[:l.rindex(':')]
            count = int(l[l.rindex(':')+1:])
            source_loc = name[name.index('<')+1:name.rindex('>')]
            stats.append((source_loc, count))
        else:
            if len(stats) > 0:
                output_stats(stats, current_count)

            name, count = l.split(':')
            current_count = int(count)
                
            current_header = name[len('num_exceptions_'):]
            print "\n\n%80s  %d" % (current_header, current_count)
            stats = []

    if len(stats) > 0:
        output_stats(stats, current_count)
