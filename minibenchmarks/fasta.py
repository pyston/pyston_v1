# -*- coding: utf-8 -*-
# The Computer Language Benchmarks Game
# http://shootout.alioth.debian.org/
#
# modified by Ian Osgood
# modified again by Heinrich Acker

import sys, bisect


# pyston change:
import hashlib
class HashOutput:
    def __init__(self):
        self.m = hashlib.md5()
    def write(self, string):
        self.m.update(string)
    def md5hash(self):
        return self.m.hexdigest()
hash_output = HashOutput()
old_stdout = sys.stdout
sys.stdout = hash_output

alu = (
   'GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGG'
   'GAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGA'
   'CCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAAT'
   'ACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCA'
   'GCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGG'
   'AGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCC'
   'AGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA')

iub = zip('acgtBDHKMNRSVWY', [0.27, 0.12, 0.12, 0.27] + [0.02]*11)

homosapiens = [
    ('a', 0.3029549426680),
    ('c', 0.1979883004921),
    ('g', 0.1975473066391),
    ('t', 0.3015094502008),
]


def genRandom(lim, ia = 3877, ic = 29573, im = 139968):
    seed = 42
    imf = float(im)
    while 1:
        seed = (seed * ia + ic) % im
        yield lim * seed / imf

Random = genRandom(1.)

def makeCumulative(table):
    P = []
    C = []
    prob = 0.
    for char, p in table:
        prob += p
        P += [prob]
        C += [char]
    return (P, C)

def repeatFasta(src, n):
    width = 60
    r = len(src)
    s = src + src + src[:n % r]
    for j in xrange(n // width):
        i = j*width % r
        print s[i:i+width]
    if n % width:
        print s[-(n % width):]

def randomFasta(table, n):
    width = 60
    r = xrange(width)
    gR = Random.next
    bb = bisect.bisect
    jn = ''.join
    probs, chars = makeCumulative(table)
    for j in xrange(n // width):
        print jn([chars[bb(probs, gR())] for i in r])
    if n % width:
        print jn([chars[bb(probs, gR())] for i in xrange(n % width)])


#n = int(sys.argv[1])
#for i in range(int(sys.argv[2])):

n = 1000

for i in range(int(1000)):
    print '>ONE Homo sapiens alu'
    repeatFasta(alu, n*2)

    print '>TWO IUB ambiguity codes'
    randomFasta(iub, n*3)

    print '>THREE Homo sapiens frequency'
    randomFasta(homosapiens, n*5)
    
sys.stdout = old_stdout
print hash_output.md5hash()
