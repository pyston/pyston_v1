import sys

_rand = 12345
def randint():
    global _rand
    _rand = (_rand * 1103515245 + 12345) % (1<<31)
    return _rand

if __name__ == "__main__":
    f = sys.stdin
    if len(sys.argv) >= 2:
        fn = sys.argv[1]
        if fn != '-':
            f = open(fn)

    T = 100
    for _T in xrange(T):
        N = 1000
        nums = [randint() for i in xrange(N)]

        r = 0
        while nums:
            m = min(nums)
            i = nums.index(m)

            r += min(i, len(nums)-1-i)
            del nums[i]

        print "Case #%d: %d" % (_T+1, r)
