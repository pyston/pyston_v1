import sys

estimated_mhz = 0

def tally(fn):
    counts = []

    for l in open(fn):
        if ':' in l:
            (key, value) = l.split(':')
            if key == "estimated_cpu_mhz":
                mhz = float(value)
            if key.startswith('us_timer_'):
                counts.append((key, int(value)))

    return (counts, mhz)

def main():
    fn1, fn2 = sys.argv[1:3]
    try:
        diff_thresh = int(sys.argv[3])
    except:
        diff_thresh = 500 # differences under 500us aren't shown

    (counts1, estimated_mhz1) = tally(fn1)
    (counts2, estimated_mhz2) = tally(fn2)

    global estimated_mhz

    if estimated_mhz1 > estimated_mhz2:
        # convert from mhz2 to mhz1
        mhz1_factor = 1
        mhz2_factor = estimated_mhz1 / estimated_mhz2
        estimated_mhz = estimated_mhz1
    else:
        # convert from mhz1 to mhz2
        mhz1_factor = estimated_mhz2 / estimated_mhz1
        mhz2_factor = 1
        estimated_mhz = estimated_mhz2


    i1 = 0
    i2 = 0
    while i1 < len(counts1) or i2 < len(counts2):
        count1 = 0
        count2 = 0

        name1 = "zzz"
        name2 = "zzz"

        if i1 < len(counts1):
            name1 = counts1[i1][0]
        if i2 < len(counts2):
            name2 = counts2[i2][0]

        if name1 < name2:
            # the counter in name1 is missing from name2, so count2 is 0
            count1 = counts1[i1][1] * mhz1_factor
            i1 += 1
        elif name1 > name2:
            # the counter in name1 is missing from name1, so count1 is 0
            count2 = counts2[i2][1] * mhz2_factor
            i2 += 1
        else:
            count1 = counts1[i1][1] * mhz1_factor
            count2 = counts2[i2][1] * mhz2_factor
            i1 += 1
            i2 += 1

        if abs(count1 - count2) < diff_thresh:
            pass # they're within threshold, do nothing
        elif count2 > count1:
            bad(format_msg(name1, count1, count2, estimated_mhz))
        else: #if count1 > count2
            good(format_msg(name1, count1, count2, estimated_mhz))

def bad(msg):
    print '\033[91m', msg, '\033[0m'

def good(msg):
    print '\033[92m', msg, '\033[0m'

def format_msg(name, before, after, mhz):
    return "%s, before %d, after %d, delta %d (approx %s)" % (name, before, after, after - before, format_delta(after - before))

def format_delta(delta):
    if abs(delta) > 1000000:
        return "%5.2f sec" % (delta / 1000000,)
    if abs(delta) > 1000:
        return "%5.2f msec" % (delta / 1000,)
    return "%5.2f usec" % (delta,)

if __name__ == "__main__":
    main()
