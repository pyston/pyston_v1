# from pypy-benchmarks/own/chaos.py, with some minor modifications
# (more output, took out the benchmark harness, disabled image saving)
#
#   Copyright (C) 2005 Carl Friedrich Bolz

"""create chaosgame-like fractals
"""

from __future__ import division

import operator
import optparse
import random
import math
random.seed(1234)
import sys
import time

class GVector(object):
    def __init__(self, x = 0, y = 0, z = 0):
        self.x = x
        self.y = y
        self.z = z

    def Mag(self):
        return math.sqrt(self.x ** 2 + self.y ** 2 + self.z ** 2)

    def dist(self, other):
        return math.sqrt((self.x - other.x) ** 2 +
                         (self.y - other.y) ** 2 +
                         (self.z - other.z) ** 2)

    def __add__(self, other):
        if not isinstance(other, GVector):
            raise ValueError, \
                    "Can't add GVector to " + str(type(other))
        v = GVector(self.x + other.x, self.y + other.y, self.z + other.z)
        return v

    def __sub__(self, other):
        return self + other * -1

    def __mul__(self, other):
        v = GVector(self.x * other, self.y * other, self.z * other)
        return v
    __rmul__ = __mul__

    def linear_combination(self, other, l1, l2=None):
        if l2 is None:
            l2 = 1 - l1
        v = GVector(self.x * l1 + other.x * l2,
                    self.y * l1 + other.y * l2,
                    self.z * l1 + other.z * l2)
        return v


    def __str__(self):
        return "<%f, %f, %f>" % (self.x, self.y, self.z)

    def __repr__(self):
        return "GVector(%f, %f, %f)" % (self.x, self.y, self.z)

def GetKnots(points, degree):
    knots = [0] * degree + range(1, len(points) - degree)
    knots += [len(points) - degree] * degree
    return knots

class Spline(object):
    """Class for representing B-Splines and NURBS of arbitrary degree"""
    def __init__(self, points, degree = 3, knots = None):
        """Creates a Spline. points is a list of GVector, degree is the
degree of the Spline."""
        if knots == None:
            self.knots = GetKnots(points, degree)
        else:
            if len(points) > len(knots) - degree + 1:
                raise ValueError, "too many control points"
            elif len(points) < len(knots) - degree + 1:
                raise ValueError, "not enough control points"
            last = knots[0]
            for cur in knots[1:]:
                if cur < last:
                    raise ValueError, \
                          "knots not strictly increasing"
                last = cur
            self.knots = knots
        self.points = points
        self.degree = degree

    def GetDomain(self):
        """Returns the domain of the B-Spline"""
        return (self.knots[self.degree - 1],
                self.knots[len(self.knots) - self.degree])

    def __call__(self, u):
        """Calculates a point of the B-Spline using de Boors Algorithm"""
        dom = self.GetDomain()
        if u < dom[0] or u > dom[1]:
            raise ValueError, "Function value not in domain"
        if u == dom[0]:
            return self.points[0]
        if u == dom[1]:
            return self.points[-1]
        I = self.GetIndex(u)
        d = [self.points[I - self.degree + 1 + ii]
             for ii in range(self.degree + 1)]
        U = self.knots
        for ik in range(1, self.degree + 1):
            for ii in range(I - self.degree + ik + 1, I + 2):
                ua = U[ii + self.degree - ik]
                ub = U[ii - 1]
                co1 = (ua - u) / (ua - ub)
                co2 = (u - ub) / (ua - ub)
                index = ii - I + self.degree - ik - 1
                d[index] = d[index].linear_combination(d[index + 1], co1, co2)
        return d[0]

    def GetIndex(self, u):
        dom = self.GetDomain()
        for ii in range(self.degree - 1, len(self.knots) - self.degree):
            if u >= self.knots[ii] and u < self.knots[ii + 1]:
                I = ii
                break
        else:
             I = dom[1] - 1
        return I

    def __len__(self):
        return len(self.points)

    def __repr__(self):
        return "Spline(%r, %r, %r)" % (self.points, self.degree, self.knots)

        
def save_im(im, fn):
    return
    f = open(fn, "wb")
    magic = 'P6\n'
    maxval = 255
    w = len(im)
    h = len(im[0])
    f.write(magic)
    f.write('%i %i\n%i\n' % (w, h, maxval))
    for j in range(h):
        for i in range(w):
            val = im[i][j]
            c = val * 255
            f.write('%c%c%c' % (c, c, c))
    f.close()

class Chaosgame(object):
    def __init__(self, splines, thickness=0.1):
        self.splines = splines
        self.thickness = thickness
        self.minx = min([p.x for spl in splines for p in spl.points])
        self.miny = min([p.y for spl in splines for p in spl.points])
        self.maxx = max([p.x for spl in splines for p in spl.points])
        self.maxy = max([p.y for spl in splines for p in spl.points])
        self.height = self.maxy - self.miny
        self.width = self.maxx - self.minx
        self.num_trafos = []
        maxlength = thickness * self.width / self.height
        for spl in splines:
            length = 0
            curr = spl(0)
            for i in range(1, 1000):
                last = curr
                t = 1 / 999 * i
                curr = spl(t)
                length += curr.dist(last)
            self.num_trafos.append(max(1, int(length / maxlength * 1.5)))
        self.num_total = reduce(operator.add, self.num_trafos, 0)


    def get_random_trafo(self):
        r = random.randrange(int(self.num_total) + 1)
        l = 0
        for i in range(len(self.num_trafos)):
            if r >= l and r < l + self.num_trafos[i]:
                return i, random.randrange(self.num_trafos[i])
            l += self.num_trafos[i]
        return len(self.num_trafos) - 1, random.randrange(self.num_trafos[-1])

    def transform_point(self, point, trafo=None):
        x = (point.x - self.minx) / self.width
        y = (point.y - self.miny) / self.height
        if trafo is None:
            trafo = self.get_random_trafo()
        start, end = self.splines[trafo[0]].GetDomain()
        length = end - start
        seg_length = length / self.num_trafos[trafo[0]]
        t = start + seg_length * trafo[1] + seg_length * x
        basepoint = self.splines[trafo[0]](t)
        if t + 1/50000 > end:
            neighbour = self.splines[trafo[0]](t - 1/50000)
            derivative = neighbour - basepoint
        else:
            neighbour = self.splines[trafo[0]](t + 1/50000)
            derivative = basepoint - neighbour
        if derivative.Mag() != 0:
            basepoint.x += derivative.y / derivative.Mag() * (y - 0.5) * \
                           self.thickness
            basepoint.y += -derivative.x / derivative.Mag() * (y - 0.5) * \
                           self.thickness
        else:
            print "r",
        self.truncate(basepoint)
        return basepoint

    def truncate(self, point):
        if point.x >= self.maxx:
            point.x = self.maxx
        if point.y >= self.maxy:
            point.y = self.maxy
        if point.x < self.minx:
            point.x = self.minx
        if point.y < self.miny:
            point.y = self.miny

    def create_image_chaos(self, w, h, name, n):
        im = [[1] * h for i in range(w)]
        point = GVector((self.maxx + self.minx) / 2,
                        (self.maxy + self.miny) / 2, 0)
        colored = 0
        times = []
        for _ in range(n):
            print _, n
            t1 = time.time()
            for i in xrange(5000):
                point = self.transform_point(point)
                x = (point.x - self.minx) / self.width * w
                y = (point.y - self.miny) / self.height * h
                x = int(x)
                y = int(y)
                if x == w:
                    x -= 1
                if y == h:
                    y -= 1
                im[x][h - y - 1] = 0
            t2 = time.time()
            times.append(t2 - t1)
        save_im(im, name)
        return times


def main(n):
    splines = [
        Spline([
            GVector(1.597350, 3.304460, 0.000000),
            GVector(1.575810, 4.123260, 0.000000),
            GVector(1.313210, 5.288350, 0.000000),
            GVector(1.618900, 5.329910, 0.000000),
            GVector(2.889940, 5.502700, 0.000000),
            GVector(2.373060, 4.381830, 0.000000),
            GVector(1.662000, 4.360280, 0.000000)],
            3, [0, 0, 0, 1, 1, 1, 2, 2, 2]),
        Spline([
            GVector(2.804500, 4.017350, 0.000000),
            GVector(2.550500, 3.525230, 0.000000),
            GVector(1.979010, 2.620360, 0.000000),
            GVector(1.979010, 2.620360, 0.000000)],
            3, [0, 0, 0, 1, 1, 1]),
        Spline([
            GVector(2.001670, 4.011320, 0.000000),
            GVector(2.335040, 3.312830, 0.000000),
            GVector(2.366800, 3.233460, 0.000000),
            GVector(2.366800, 3.233460, 0.000000)],
            3, [0, 0, 0, 1, 1, 1])
        ]
    c = Chaosgame(splines, 0.25)
    return c.create_image_chaos(1000, 1200, "py.ppm", n)


if __name__ == "__main__":
    main(100)
