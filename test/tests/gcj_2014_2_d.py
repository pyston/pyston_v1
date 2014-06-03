# expected: fail
# - need to support closures

import sys

P = 1000000007

def powmod(b, e):
    r = 1
    m = b
    b = 1
    while e:
        if e & b:
            r = (r * m) % P
            e &= ~b
        m = (m * m) % P
        b <<= 1
    return r

def inv(n):
    return powmod(n, P - 2)

def choose(n, c):
    t = 1
    for i in xrange(c + 1, n + 1):
        t = (t * i) % P
    for i in xrange(1, n - c + 1):
        t = (t * inv(i)) % P
    return t

for i in xrange(1, 1000):
    assert (inv(i) * i) % P == 1

if __name__ == "__main__":
    # test data is from the Google CodeJam site:
    s = """
5
20 16
IHAHAG
NPOEMADQGIHEB
IEDKEQCIOKKAKI
NHGMBMAINMQNGF
CDJPIQIGAJJOIQNAQGJI
JAMAPACLCHIPNBKAPG
DBBLEB
AIGIIPOQFK
FQHEBPIEADL
AADGDIGQQNKMPGK
ONJD
NH
ABMGHGHJACBJNIEJCAF
DQMELMNHNLHPNBFFQK
PCKPOQONKELNNDFBCE
JPLLNEECCMHPFDBJHEOJHJ
IMBPEMNPQLFOPB
PCNGQOENGKHJPDNAFIQLFCMO
AKCAOKP
OCGFQAAOOBFCOGNGKDM
17 10
GDICD
ABAEGLKABNCLKGGJJCAL
IDAIKCJHF
BNCFDDEBJ
CCJDBIHLMHGBAHMN
CMDFICIKHFKFJKBEIKI
IJJDCJFJ
LLMGGGHI
C
BEMECLEGBNBCNMEKJIB
HJCEFCG
DBNEKNIHIDLBNELBHADNGJ
DDCBJ
EDKGLCLGFKGKGFCMNFJ
DLHEEGGEKNEADN
EEENAMDBMLMLEJGCNNHGFBAM
AMHGBBCGFKLMEKCMGK
6 4
QMNCVNWWJCXHDHDKNARYXLP
DWHXQHJVPQDVO
QKKS
TRRIFUGURDFVDOALTMBUFCSST
LEIGFEXXOESUBMAGIDWKQRXR
KU
7 4
CDECGBCCFEDCA
BFFEB
EECGDCDECEAADFABCAFCDEG
CFGCCCDFADAF
BECCEAFGDADDBBBE
BGGCDDAADGEFCBCEDCGBEDA
EGFDGAGCDEFE
12 9
JNHNTURARPLCPJUEHPJLR
ML
SOQPUECHV
BEKHQCTBNFVLC
MGQARMIIDAPPJBUPG
ABWODFJJSEEHCKLIBU
IKFNEKCERIGUSS
LTMBPKRQJDVAQEBJVSPHUSBM
UKJOOQUSFGUOQRUNWVCMHQO
QQKRPHMRLALSAGLIDKLHFP
LAGNBBSVFEMUODPTKGEUSCKRA
INUVNMJPLMFSCSJNTUWHWOKO
""".strip()
    l = s.split('\n')

    T = int(l.pop(0))
    for _T in xrange(T):
        M, N = map(int, l.pop(0).split())

        strings = []
        suffix_map = {}
        prefix_counts = {}
        for i in xrange(M):
            s = l.pop(0).strip()
            strings.append(s)

            for i in xrange(len(s) + 1):
                substr = s[:i]
                prefix_counts[substr] = prefix_counts.get(substr, 0) + 1

            for i in xrange(len(s)):
                substr1 = s[:i]
                substr2 = s[:i+1]
                suffix_map.setdefault(substr1, set([])).add(substr2)
            suffix_map.setdefault(s, set([])).add(None)

        worst = 0
        for n in prefix_counts.values():
            worst += min(n, N)

        total = powmod(N, M)

        mem = {}
        def ftotal(into, exist):
            # print N, into, exist

            if into < max(exist):
                return 0

            key = (into, tuple(sorted(exist)))
            if key in mem:
                return mem[key]

            bottom = 1
            for e in exist:
                bottom = (bottom * choose(into, e)) % P
            bad = 0
            # print bottom, "recursing:"
            for i in xrange(1, into):
                pbad = ftotal(i, exist)
                # print i, choose(into, i), pbad
                bad += choose(into, i) * pbad
            # print bottom, bad
            rtn = bottom - bad
            mem[key] = rtn
            return rtn

        def frac(into, exist):
            into = min(into, N)
            exist = [min(e, N) for e in exist]

            if into < max(exist):
                return 0
            if into == max(exist):
                return 1

            top = ftotal(into, exist) * choose(N, into)
            bottom = 1
            for e in exist:
                bottom = (bottom * choose(N, e)) % P
            # print top, bottom
            return (top * inv(bottom)) % P

        for prefix in prefix_counts:
            suffix_counts = []
            for suffix in suffix_map[prefix]:
                if suffix is None:
                    suffix_counts.append(1)
                else:
                    suffix_counts.append(prefix_counts[suffix])

            fr = frac(prefix_counts[prefix], suffix_counts)
            # print prefix, prefix_counts[prefix], suffix_counts, (fr * 81) % P, 81
            total = (total * fr) % P

        # print prefix_counts
        print "Case #%d: %d %d" % (_T+1, worst, total % P)

