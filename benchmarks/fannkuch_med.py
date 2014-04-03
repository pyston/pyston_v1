import time

def fannkuch(n):
    count = range(1, n+1)
    max_flips = 0
    m = n-1
    r = n
    check = 0
    perm1 = range(n)
    perm = range(n)
    perm1_ins = perm1.insert
    perm1_pop = perm1.pop

    while 1:
        if check < 30:
            #print "".join(str(i+1) for i in perm1)
            check = check + 1

        while r != 1:
            count[r-1] = r
            r = r - 1

        if perm1[0] != 0 and perm1[m] != m:
            perm = perm1[:]
            flips_count = 0
            k = perm[0]
            while k:
                perm[:k+1] = perm[k::-1]
                flips_count = flips_count + 1
                k = perm[0]

            if flips_count > max_flips:
                max_flips = flips_count

        while r != n:
            perm1_ins(r, perm1_pop(0))
            count[r] = count[r] - 1
            if count[r] > 0:
                break
            r = r + 1
        else:
            return max_flips

DEFAULT_ARG = 9

def main(n):
    for i in range(2, n):
        t0 = time.time()
        print fannkuch(i)
        tk = time.time()
        print tk - t0
main(10)

