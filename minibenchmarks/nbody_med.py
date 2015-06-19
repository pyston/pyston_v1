# -*- coding: utf-8 -*-
# The Computer Language Benchmarks Game
# http://shootout.alioth.debian.org/
#
# contributed by Kevin Carson
# modified by Tupteq, Fredrik Johansson, and Daniel Nanz

import time

def combinations(l):
    result = []
    for x in xrange(len(l) - 1):
        for j in xrange(x+1, len(l)):
        # ls = l[x+1:]
        # for y in ls:
            y = l[j]
            result.append((l[x],y))
    return result

PI = 3.14159265358979323
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24

BODIES = {
1: ([0.0, 0.0, 0.0], [0.0, 0.0, 0.0], SOLAR_MASS),

2: ([4.84143144246472090e+00,
                 -1.16032004402742839e+00,
                 -1.03622044471123109e-01],
                [1.66007664274403694e-03 * DAYS_PER_YEAR,
                 7.69901118419740425e-03 * DAYS_PER_YEAR,
                 -6.90460016972063023e-05 * DAYS_PER_YEAR],
                9.54791938424326609e-04 * SOLAR_MASS),

3: ([8.34336671824457987e+00,
                4.12479856412430479e+00,
                -4.03523417114321381e-01],
               [-2.76742510726862411e-03 * DAYS_PER_YEAR,
                4.99852801234917238e-03 * DAYS_PER_YEAR,
                2.30417297573763929e-05 * DAYS_PER_YEAR],
               2.85885980666130812e-04 * SOLAR_MASS),

4: ([1.28943695621391310e+01,
                -1.51111514016986312e+01,
                -2.23307578892655734e-01],
               [2.96460137564761618e-03 * DAYS_PER_YEAR,
                2.37847173959480950e-03 * DAYS_PER_YEAR,
                -2.96589568540237556e-05 * DAYS_PER_YEAR],
               4.36624404335156298e-05 * SOLAR_MASS),

5: ([1.53796971148509165e+01,
                 -2.59193146099879641e+01,
                 1.79258772950371181e-01],
                [2.68067772490389322e-03 * DAYS_PER_YEAR,
                 1.62824170038242295e-03 * DAYS_PER_YEAR,
                 -9.51592254519715870e-05 * DAYS_PER_YEAR],
                5.15138902046611451e-05 * SOLAR_MASS) }


SYSTEM = BODIES.values()
PAIRS = combinations(SYSTEM)


def advance(dt, n):
    bodies=SYSTEM
    pairs=PAIRS

    for i in xrange(n):
        for (((x1, y1, z1), v1, m1),
             ((x2, y2, z2), v2, m2)) in pairs:
            dx = x1 - x2
            dy = y1 - y2
            dz = z1 - z2
            mag = dt * ((dx * dx + dy * dy + dz * dz) ** (-1.5))
            b1m = m1 * mag
            b2m = m2 * mag
            v1[0] = v1[0] - dx * b2m
            v1[1] = v1[1] - dy * b2m
            v1[2] = v1[2] - dz * b2m
            v2[0] = v2[0] + dx * b1m
            v2[1] = v2[1] + dy * b1m
            v2[2] = v2[2] + dz * b1m
        for (r, (vx, vy, vz), m) in bodies:
            r[0] = r[0] + dt * vx
            r[1] = r[1] + dt * vy
            r[2] = r[2] + dt * vz


def report_energy():
    bodies=SYSTEM
    pairs=PAIRS
    e=0.0
    for (((x1, y1, z1), v1, m1),
         ((x2, y2, z2), v2, m2)) in pairs:
        dx = x1 - x2
        dy = y1 - y2
        dz = z1 - z2
        e = e - (m1 * m2) / ((dx * dx + dy * dy + dz * dz) ** 0.5)
    for (r, (vx, vy, vz), m) in bodies:
        e = e + m * (vx * vx + vy * vy + vz * vz) / 2.
    return e

def offset_momentum(ref, bodies, px, py, pz):

    for (r, (vx, vy, vz), m) in bodies:
        px = px - vx * m
        py = py - vy * m
        pz = pz - vz * m
    (r, v, m) = ref
    v[0] = px / m
    v[1] = py / m
    v[2] = pz / m

NUMBER_OF_ITERATIONS = 5000

def main(n, ref):
    # XXX warmup
    
    times = []
    for i in range(n):
        t0 = time.time()
        offset_momentum(BODIES[ref], SYSTEM, 0.0, 0.0, 0.0)
        e1 = report_energy()
        advance(0.01, NUMBER_OF_ITERATIONS)
        print e1 - report_energy()
        tk = time.time()
        times.append(tk - t0)
    return times

main(40,2)


