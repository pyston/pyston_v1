import sys

def tally(fn):
    counts = {}

    for l in open(fn):
        samples = int(l.split()[3])
        func = l.split()[-1]
        counts[func] = samples

    return counts

def main():
    fn1, fn2 = sys.argv[1:3]

    counts1 = tally(fn1)
    counts2 = tally(fn2)

    total1 = sum(counts1.values())
    total2 = sum(counts2.values())

    names = list(set(counts1.keys()) | set(counts2.keys()))

    diff_thresh = (total1 + total2) / 2 / 100

    names.sort(key=lambda n: counts1.get(n, 0) + counts2.get(n, 0), reverse=True)
    for n in names:
        c1 = counts1.get(n, 0)
        c2 = counts2.get(n, 0)
        if abs(c1 - c2) >= diff_thresh:
            print n, counts1.get(n, 0), counts2.get(n, 0)

if __name__ == "__main__":
    main()
