import time

PIDIGITS_LEN = 1500

def pidigits(length):
    i = k = ns = 0
    k1 = 1
    n,a,d,t,u = 1,0,1,0,0
    while(True):
        k += 1
        t = n<<1
        n *= k
        a += t
        k1 += 2
        a *= k1
        d *= k1
        if a >= n:
            t,u = divmod(n*3 + a,d)
            u += n
            if d > u:
                ns = ns*10 + t
                i += 1
                if i % 10 == 0:
                    ns = 0
                if i >= length:
                    break
                a -= d*t
                a *= 10
                n *= 10

def main(n):
    l = []
    for i in range(n):
        t0 = time.time()
        pidigits(PIDIGITS_LEN)
        l.append(time.time() - t0)
    return l

main(100)
