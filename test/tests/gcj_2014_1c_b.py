# expected: fail
# - working on it

import sys

def compact(s):
    i = 0
    while i < len(s) - 1:
        if s[i] == s[i+1]:
            s = s[:i] + s[i+1:]
        else:
            i += 1
    return s

# TODO This should be a subclass of Exception not object:
class NotPossible(object):
    pass

P = 1000000007

def fact(n):
    t = 1
    for i in xrange(1, n+1):
        t = (t * i) % P
    return t

if __name__ == "__main__":
    # test data is from the Google CodeJam site:
    s = """
3
3
ab bbbc cd
4
aa aa bc c
2
abc bcd
    """.strip()

    l = s.split('\n')
    T = int(l.pop(0))
    for _T in xrange(T):
        N = int(l.pop(0))
        trains = l.pop(0).split()
        trains = map(compact, trains)

        try:
            for s in trains:
                if s[0] in s[1:]:
                    raise NotPossible
                if s[-1] in s[:-1]:
                    raise NotPossible
                for c in s[1:-1]:
                    cnt = sum([s2.count(c) for s2 in trains])
                    assert cnt >= 1
                    if cnt != 1:
                        raise NotPossible()

            # print trains
            singles = {}
            chunks = []
            for i in xrange(N):
                if len(trains[i]) == 1:
                    singles[trains[i]] = singles.get(trains[i], 0) + 1
                else:
                    chunks.append(trains[i][0] + trains[i][-1])
            # print singles, chunks

            mult = 1
            left = 0
            while chunks:
                # print mult, left, singles, chunks
                first = chunks.pop()
                assert len(set(first)) == len(first)

                mult = (mult * fact(singles.pop(first[0], 0))) % P
                mult = (mult * fact(singles.pop(first[-1], 0))) % P

                for ch in chunks:
                    assert len(set(ch)) == len(ch)
                    if ch[0] in first:
                        if ch[0] in first[:-1]:
                            raise NotPossible()
                        # assert not any(c == ch[0] for c in ch[1:])
                        if any([c in first for c in ch[1:]]):
                            raise NotPossible()
                        assert ch[0] == first[-1]
                        chunks.remove(ch)
                        chunks.append(first + ch[1:])
                        break
                    if ch[-1] in first:
                        if ch[-1] in first[1:]:
                            raise NotPossible()
                        # assert not any(c == ch[-1] for c in ch[:-1])
                        if any([c in first for c in ch[:-1]]):
                            raise NotPossible()
                        assert ch[-1] == first[0]
                        chunks.remove(ch)
                        chunks.append(ch + first[1:])
                        break
                else:
                    left += 1
                    continue
            # print mult, left, singles, chunks

            for k, v in singles.iteritems():
                left += 1
                mult = (mult * fact(v)) % P

            assert left >= 0
            while left:
                mult = (mult * left) % P
                left = left - 1

            print "Case #%d: %d" % (_T+1, mult)
        except NotPossible:
            print "Case #%d: 0" % (_T+1,)
    assert not l
