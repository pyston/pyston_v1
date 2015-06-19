# expected: fail
import unittest
import random
import time
import pickle
import warnings
from math import log, exp, pi, fsum, sin
from functools import reduce
from test import test_support

class TestBasicOps(unittest.TestCase):
    # Superclass with tests common to all generators.
    # Subclasses must arrange for self.gen to retrieve the Random instance
    # to be tested.

    def randomlist(self, n):
        """Helper function to make a list of random numbers"""
        return [self.gen.random() for i in xrange(n)]

    def test_autoseed(self):
        self.gen.seed()
        state1 = self.gen.getstate()
        time.sleep(0.1)
        self.gen.seed()      # diffent seeds at different times
        state2 = self.gen.getstate()
        self.assertNotEqual(state1, state2)

    def test_saverestore(self):
        N = 1000
        self.gen.seed()
        state = self.gen.getstate()
        randseq = self.randomlist(N)
        self.gen.setstate(state)    # should regenerate the same sequence
        self.assertEqual(randseq, self.randomlist(N))

    def test_seedargs(self):
        for arg in [None, 0, 0L, 1, 1L, -1, -1L, 10**20, -(10**20),
                    3.14, 1+2j, 'a', tuple('abc')]:
            self.gen.seed(arg)
        for arg in [range(3), dict(one=1)]:
            self.assertRaises(TypeError, self.gen.seed, arg)
        self.assertRaises(TypeError, self.gen.seed, 1, 2)
        self.assertRaises(TypeError, type(self.gen), [])

    def test_jumpahead(self):
        self.gen.seed()
        state1 = self.gen.getstate()
        self.gen.jumpahead(100)
        state2 = self.gen.getstate()    # s/b distinct from state1
        self.assertNotEqual(state1, state2)
        self.gen.jumpahead(100)
        state3 = self.gen.getstate()    # s/b distinct from state2
        self.assertNotEqual(state2, state3)

        with test_support.check_py3k_warnings(quiet=True):
            self.assertRaises(TypeError, self.gen.jumpahead)  # needs an arg
            self.assertRaises(TypeError, self.gen.jumpahead, 2, 3)  # too many

    def test_jumpahead_produces_valid_state(self):
        # From http://bugs.python.org/issue14591.
        self.gen.seed(199210368)
        self.gen.jumpahead(13550674232554645900)
        for i in range(500):
            val = self.gen.random()
            self.assertLess(val, 1.0)

    def test_sample(self):
        # For the entire allowable range of 0 <= k <= N, validate that
        # the sample is of the correct length and contains only unique items
        N = 100
        population = xrange(N)
        for k in xrange(N+1):
            s = self.gen.sample(population, k)
            self.assertEqual(len(s), k)
            uniq = set(s)
            self.assertEqual(len(uniq), k)
            self.assertTrue(uniq <= set(population))
        self.assertEqual(self.gen.sample([], 0), [])  # test edge case N==k==0

    def test_sample_distribution(self):
        # For the entire allowable range of 0 <= k <= N, validate that
        # sample generates all possible permutations
        n = 5
        pop = range(n)
        trials = 10000  # large num prevents false negatives without slowing normal case
        def factorial(n):
            return reduce(int.__mul__, xrange(1, n), 1)
        for k in xrange(n):
            expected = factorial(n) // factorial(n-k)
            perms = {}
            for i in xrange(trials):
                perms[tuple(self.gen.sample(pop, k))] = None
                if len(perms) == expected:
                    break
            else:
                self.fail()

    def test_sample_inputs(self):
        # SF bug #801342 -- population can be any iterable defining __len__()
        self.gen.sample(set(range(20)), 2)
        self.gen.sample(range(20), 2)
        self.gen.sample(xrange(20), 2)
        self.gen.sample(str('abcdefghijklmnopqrst'), 2)
        self.gen.sample(tuple('abcdefghijklmnopqrst'), 2)

    def test_sample_on_dicts(self):
        self.gen.sample(dict.fromkeys('abcdefghijklmnopqrst'), 2)

        # SF bug #1460340 -- random.sample can raise KeyError
        a = dict.fromkeys(range(10)+range(10,100,2)+range(100,110))
        self.gen.sample(a, 3)

        # A followup to bug #1460340:  sampling from a dict could return
        # a subset of its keys or of its values, depending on the size of
        # the subset requested.
        N = 30
        d = dict((i, complex(i, i)) for i in xrange(N))
        for k in xrange(N+1):
            samp = self.gen.sample(d, k)
            # Verify that we got ints back (keys); the values are complex.
            for x in samp:
                self.assertTrue(type(x) is int)
        samp.sort()
        self.assertEqual(samp, range(N))

    def test_gauss(self):
        # Ensure that the seed() method initializes all the hidden state.  In
        # particular, through 2.2.1 it failed to reset a piece of state used
        # by (and only by) the .gauss() method.

        for seed in 1, 12, 123, 1234, 12345, 123456, 654321:
            self.gen.seed(seed)
            x1 = self.gen.random()
            y1 = self.gen.gauss(0, 1)

            self.gen.seed(seed)
            x2 = self.gen.random()
            y2 = self.gen.gauss(0, 1)

            self.assertEqual(x1, x2)
            self.assertEqual(y1, y2)

    def test_pickling(self):
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            state = pickle.dumps(self.gen, proto)
            origseq = [self.gen.random() for i in xrange(10)]
            newgen = pickle.loads(state)
            restoredseq = [newgen.random() for i in xrange(10)]
            self.assertEqual(origseq, restoredseq)

    def test_bug_1727780(self):
        # verify that version-2-pickles can be loaded
        # fine, whether they are created on 32-bit or 64-bit
        # platforms, and that version-3-pickles load fine.
        files = [("randv2_32.pck", 780),
                 ("randv2_64.pck", 866),
                 ("randv3.pck", 343)]
        for file, value in files:
            f = open(test_support.findfile(file),"rb")
            r = pickle.load(f)
            f.close()
            self.assertEqual(r.randrange(1000), value)

class WichmannHill_TestBasicOps(TestBasicOps):
    gen = random.WichmannHill()

    def test_setstate_first_arg(self):
        self.assertRaises(ValueError, self.gen.setstate, (2, None, None))

    def test_strong_jumpahead(self):
        # tests that jumpahead(n) semantics correspond to n calls to random()
        N = 1000
        s = self.gen.getstate()
        self.gen.jumpahead(N)
        r1 = self.gen.random()
        # now do it the slow way
        self.gen.setstate(s)
        for i in xrange(N):
            self.gen.random()
        r2 = self.gen.random()
        self.assertEqual(r1, r2)

    def test_gauss_with_whseed(self):
        # Ensure that the seed() method initializes all the hidden state.  In
        # particular, through 2.2.1 it failed to reset a piece of state used
        # by (and only by) the .gauss() method.

        for seed in 1, 12, 123, 1234, 12345, 123456, 654321:
            self.gen.whseed(seed)
            x1 = self.gen.random()
            y1 = self.gen.gauss(0, 1)

            self.gen.whseed(seed)
            x2 = self.gen.random()
            y2 = self.gen.gauss(0, 1)

            self.assertEqual(x1, x2)
            self.assertEqual(y1, y2)

    def test_bigrand(self):
        # Verify warnings are raised when randrange is too large for random()
        with warnings.catch_warnings():
            warnings.filterwarnings("error", "Underlying random")
            self.assertRaises(UserWarning, self.gen.randrange, 2**60)

class SystemRandom_TestBasicOps(TestBasicOps):
    gen = random.SystemRandom()

    def test_autoseed(self):
        # Doesn't need to do anything except not fail
        self.gen.seed()

    def test_saverestore(self):
        self.assertRaises(NotImplementedError, self.gen.getstate)
        self.assertRaises(NotImplementedError, self.gen.setstate, None)

    def test_seedargs(self):
        # Doesn't need to do anything except not fail
        self.gen.seed(100)

    def test_jumpahead(self):
        # Doesn't need to do anything except not fail
        self.gen.jumpahead(100)

    def test_gauss(self):
        self.gen.gauss_next = None
        self.gen.seed(100)
        self.assertEqual(self.gen.gauss_next, None)

    def test_pickling(self):
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            self.assertRaises(NotImplementedError, pickle.dumps, self.gen, proto)

    def test_53_bits_per_float(self):
        # This should pass whenever a C double has 53 bit precision.
        span = 2 ** 53
        cum = 0
        for i in xrange(100):
            cum |= int(self.gen.random() * span)
        self.assertEqual(cum, span-1)

    def test_bigrand(self):
        # The randrange routine should build-up the required number of bits
        # in stages so that all bit positions are active.
        span = 2 ** 500
        cum = 0
        for i in xrange(100):
            r = self.gen.randrange(span)
            self.assertTrue(0 <= r < span)
            cum |= r
        self.assertEqual(cum, span-1)

    def test_bigrand_ranges(self):
        for i in [40,80, 160, 200, 211, 250, 375, 512, 550]:
            start = self.gen.randrange(2 ** (i-2))
            stop = self.gen.randrange(2 ** i)
            if stop <= start:
                continue
            self.assertTrue(start <= self.gen.randrange(start, stop) < stop)

    def test_rangelimits(self):
        for start, stop in [(-2,0), (-(2**60)-2,-(2**60)), (2**60,2**60+2)]:
            self.assertEqual(set(range(start,stop)),
                set([self.gen.randrange(start,stop) for i in xrange(100)]))

    def test_genrandbits(self):
        # Verify ranges
        for k in xrange(1, 1000):
            self.assertTrue(0 <= self.gen.getrandbits(k) < 2**k)

        # Verify all bits active
        getbits = self.gen.getrandbits
        for span in [1, 2, 3, 4, 31, 32, 32, 52, 53, 54, 119, 127, 128, 129]:
            cum = 0
            for i in xrange(100):
                cum |= getbits(span)
            self.assertEqual(cum, 2**span-1)

        # Verify argument checking
        self.assertRaises(TypeError, self.gen.getrandbits)
        self.assertRaises(TypeError, self.gen.getrandbits, 1, 2)
        self.assertRaises(ValueError, self.gen.getrandbits, 0)
        self.assertRaises(ValueError, self.gen.getrandbits, -1)
        self.assertRaises(TypeError, self.gen.getrandbits, 10.1)

    def test_randbelow_logic(self, _log=log, int=int):
        # check bitcount transition points:  2**i and 2**(i+1)-1
        # show that: k = int(1.001 + _log(n, 2))
        # is equal to or one greater than the number of bits in n
        for i in xrange(1, 1000):
            n = 1L << i # check an exact power of two
            numbits = i+1
            k = int(1.00001 + _log(n, 2))
            self.assertEqual(k, numbits)
            self.assertTrue(n == 2**(k-1))

            n += n - 1      # check 1 below the next power of two
            k = int(1.00001 + _log(n, 2))
            self.assertIn(k, [numbits, numbits+1])
            self.assertTrue(2**k > n > 2**(k-2))

            n -= n >> 15     # check a little farther below the next power of two
            k = int(1.00001 + _log(n, 2))
            self.assertEqual(k, numbits)        # note the stronger assertion
            self.assertTrue(2**k > n > 2**(k-1))   # note the stronger assertion


class MersenneTwister_TestBasicOps(TestBasicOps):
    gen = random.Random()

    def test_setstate_first_arg(self):
        self.assertRaises(ValueError, self.gen.setstate, (1, None, None))

    def test_setstate_middle_arg(self):
        # Wrong type, s/b tuple
        self.assertRaises(TypeError, self.gen.setstate, (2, None, None))
        # Wrong length, s/b 625
        self.assertRaises(ValueError, self.gen.setstate, (2, (1,2,3), None))
        # Wrong type, s/b tuple of 625 ints
        self.assertRaises(TypeError, self.gen.setstate, (2, ('a',)*625, None))
        # Last element s/b an int also
        self.assertRaises(TypeError, self.gen.setstate, (2, (0,)*624+('a',), None))

    def test_referenceImplementation(self):
        # Compare the python implementation with results from the original
        # code.  Create 2000 53-bit precision random floats.  Compare only
        # the last ten entries to show that the independent implementations
        # are tracking.  Here is the main() function needed to create the
        # list of expected random numbers:
        #    void main(void){
        #         int i;
        #         unsigned long init[4]={61731, 24903, 614, 42143}, length=4;
        #         init_by_array(init, length);
        #         for (i=0; i<2000; i++) {
        #           printf("%.15f ", genrand_res53());
        #           if (i%5==4) printf("\n");
        #         }
        #     }
        expected = [0.45839803073713259,
                    0.86057815201978782,
                    0.92848331726782152,
                    0.35932681119782461,
                    0.081823493762449573,
                    0.14332226470169329,
                    0.084297823823520024,
                    0.53814864671831453,
                    0.089215024911993401,
                    0.78486196105372907]

        self.gen.seed(61731L + (24903L<<32) + (614L<<64) + (42143L<<96))
        actual = self.randomlist(2000)[-10:]
        for a, e in zip(actual, expected):
            self.assertAlmostEqual(a,e,places=14)

    def test_strong_reference_implementation(self):
        # Like test_referenceImplementation, but checks for exact bit-level
        # equality.  This should pass on any box where C double contains
        # at least 53 bits of precision (the underlying algorithm suffers
        # no rounding errors -- all results are exact).
        from math import ldexp

        expected = [0x0eab3258d2231fL,
                    0x1b89db315277a5L,
                    0x1db622a5518016L,
                    0x0b7f9af0d575bfL,
                    0x029e4c4db82240L,
                    0x04961892f5d673L,
                    0x02b291598e4589L,
                    0x11388382c15694L,
                    0x02dad977c9e1feL,
                    0x191d96d4d334c6L]
        self.gen.seed(61731L + (24903L<<32) + (614L<<64) + (42143L<<96))
        actual = self.randomlist(2000)[-10:]
        for a, e in zip(actual, expected):
            self.assertEqual(long(ldexp(a, 53)), e)

    def test_long_seed(self):
        # This is most interesting to run in debug mode, just to make sure
        # nothing blows up.  Under the covers, a dynamically resized array
        # is allocated, consuming space proportional to the number of bits
        # in the seed.  Unfortunately, that's a quadratic-time algorithm,
        # so don't make this horribly big.
        seed = (1L << (10000 * 8)) - 1  # about 10K bytes
        self.gen.seed(seed)

    def test_53_bits_per_float(self):
        # This should pass whenever a C double has 53 bit precision.
        span = 2 ** 53
        cum = 0
        for i in xrange(100):
            cum |= int(self.gen.random() * span)
        self.assertEqual(cum, span-1)

    def test_bigrand(self):
        # The randrange routine should build-up the required number of bits
        # in stages so that all bit positions are active.
        span = 2 ** 500
        cum = 0
        for i in xrange(100):
            r = self.gen.randrange(span)
            self.assertTrue(0 <= r < span)
            cum |= r
        self.assertEqual(cum, span-1)

    def test_bigrand_ranges(self):
        for i in [40,80, 160, 200, 211, 250, 375, 512, 550]:
            start = self.gen.randrange(2 ** (i-2))
            stop = self.gen.randrange(2 ** i)
            if stop <= start:
                continue
            self.assertTrue(start <= self.gen.randrange(start, stop) < stop)

    def test_rangelimits(self):
        for start, stop in [(-2,0), (-(2**60)-2,-(2**60)), (2**60,2**60+2)]:
            self.assertEqual(set(range(start,stop)),
                set([self.gen.randrange(start,stop) for i in xrange(100)]))

    def test_genrandbits(self):
        # Verify cross-platform repeatability
        self.gen.seed(1234567)
        self.assertEqual(self.gen.getrandbits(100),
                         97904845777343510404718956115L)
        # Verify ranges
        for k in xrange(1, 1000):
            self.assertTrue(0 <= self.gen.getrandbits(k) < 2**k)

        # Verify all bits active
        getbits = self.gen.getrandbits
        for span in [1, 2, 3, 4, 31, 32, 32, 52, 53, 54, 119, 127, 128, 129]:
            cum = 0
            for i in xrange(100):
                cum |= getbits(span)
            self.assertEqual(cum, 2**span-1)

        # Verify argument checking
        self.assertRaises(TypeError, self.gen.getrandbits)
        self.assertRaises(TypeError, self.gen.getrandbits, 'a')
        self.assertRaises(TypeError, self.gen.getrandbits, 1, 2)
        self.assertRaises(ValueError, self.gen.getrandbits, 0)
        self.assertRaises(ValueError, self.gen.getrandbits, -1)

    def test_randbelow_logic(self, _log=log, int=int):
        # check bitcount transition points:  2**i and 2**(i+1)-1
        # show that: k = int(1.001 + _log(n, 2))
        # is equal to or one greater than the number of bits in n
        for i in xrange(1, 1000):
            n = 1L << i # check an exact power of two
            numbits = i+1
            k = int(1.00001 + _log(n, 2))
            self.assertEqual(k, numbits)
            self.assertTrue(n == 2**(k-1))

            n += n - 1      # check 1 below the next power of two
            k = int(1.00001 + _log(n, 2))
            self.assertIn(k, [numbits, numbits+1])
            self.assertTrue(2**k > n > 2**(k-2))

            n -= n >> 15     # check a little farther below the next power of two
            k = int(1.00001 + _log(n, 2))
            self.assertEqual(k, numbits)        # note the stronger assertion
            self.assertTrue(2**k > n > 2**(k-1))   # note the stronger assertion

    def test_randrange_bug_1590891(self):
        start = 1000000000000
        stop = -100000000000000000000
        step = -200
        x = self.gen.randrange(start, stop, step)
        self.assertTrue(stop < x <= start)
        self.assertEqual((x+stop)%step, 0)

def gamma(z, sqrt2pi=(2.0*pi)**0.5):
    # Reflection to right half of complex plane
    if z < 0.5:
        return pi / sin(pi*z) / gamma(1.0-z)
    # Lanczos approximation with g=7
    az = z + (7.0 - 0.5)
    return az ** (z-0.5) / exp(az) * sqrt2pi * fsum([
        0.9999999999995183,
        676.5203681218835 / z,
        -1259.139216722289 / (z+1.0),
        771.3234287757674 / (z+2.0),
        -176.6150291498386 / (z+3.0),
        12.50734324009056 / (z+4.0),
        -0.1385710331296526 / (z+5.0),
        0.9934937113930748e-05 / (z+6.0),
        0.1659470187408462e-06 / (z+7.0),
    ])

class TestDistributions(unittest.TestCase):
    def test_zeroinputs(self):
        # Verify that distributions can handle a series of zero inputs'
        g = random.Random()
        x = [g.random() for i in xrange(50)] + [0.0]*5
        g.random = x[:].pop; g.uniform(1,10)
        g.random = x[:].pop; g.paretovariate(1.0)
        g.random = x[:].pop; g.expovariate(1.0)
        g.random = x[:].pop; g.weibullvariate(1.0, 1.0)
        g.random = x[:].pop; g.vonmisesvariate(1.0, 1.0)
        g.random = x[:].pop; g.normalvariate(0.0, 1.0)
        g.random = x[:].pop; g.gauss(0.0, 1.0)
        g.random = x[:].pop; g.lognormvariate(0.0, 1.0)
        g.random = x[:].pop; g.vonmisesvariate(0.0, 1.0)
        g.random = x[:].pop; g.gammavariate(0.01, 1.0)
        g.random = x[:].pop; g.gammavariate(1.0, 1.0)
        g.random = x[:].pop; g.gammavariate(200.0, 1.0)
        g.random = x[:].pop; g.betavariate(3.0, 3.0)
        g.random = x[:].pop; g.triangular(0.0, 1.0, 1.0/3.0)

    def test_avg_std(self):
        # Use integration to test distribution average and standard deviation.
        # Only works for distributions which do not consume variates in pairs
        g = random.Random()
        N = 5000
        x = [i/float(N) for i in xrange(1,N)]
        for variate, args, mu, sigmasqrd in [
                (g.uniform, (1.0,10.0), (10.0+1.0)/2, (10.0-1.0)**2/12),
                (g.triangular, (0.0, 1.0, 1.0/3.0), 4.0/9.0, 7.0/9.0/18.0),
                (g.expovariate, (1.5,), 1/1.5, 1/1.5**2),
                (g.vonmisesvariate, (1.23, 0), pi, pi**2/3),
                (g.paretovariate, (5.0,), 5.0/(5.0-1),
                                  5.0/((5.0-1)**2*(5.0-2))),
                (g.weibullvariate, (1.0, 3.0), gamma(1+1/3.0),
                                  gamma(1+2/3.0)-gamma(1+1/3.0)**2) ]:
            g.random = x[:].pop
            y = []
            for i in xrange(len(x)):
                try:
                    y.append(variate(*args))
                except IndexError:
                    pass
            s1 = s2 = 0
            for e in y:
                s1 += e
                s2 += (e - mu) ** 2
            N = len(y)
            self.assertAlmostEqual(s1/N, mu, places=2,
                                   msg='%s%r' % (variate.__name__, args))
            self.assertAlmostEqual(s2/(N-1), sigmasqrd, places=2,
                                   msg='%s%r' % (variate.__name__, args))

    def test_constant(self):
        g = random.Random()
        N = 100
        for variate, args, expected in [
                (g.uniform, (10.0, 10.0), 10.0),
                (g.triangular, (10.0, 10.0), 10.0),
                (g.triangular, (10.0, 10.0, 10.0), 10.0),
                (g.expovariate, (float('inf'),), 0.0),
                (g.vonmisesvariate, (3.0, float('inf')), 3.0),
                (g.gauss, (10.0, 0.0), 10.0),
                (g.lognormvariate, (0.0, 0.0), 1.0),
                (g.lognormvariate, (-float('inf'), 0.0), 0.0),
                (g.normalvariate, (10.0, 0.0), 10.0),
                (g.paretovariate, (float('inf'),), 1.0),
                (g.weibullvariate, (10.0, float('inf')), 10.0),
                (g.weibullvariate, (0.0, 10.0), 0.0),
            ]:
            for i in range(N):
                self.assertEqual(variate(*args), expected)

    def test_von_mises_range(self):
        # Issue 17149: von mises variates were not consistently in the
        # range [0, 2*PI].
        g = random.Random()
        N = 100
        for mu in 0.0, 0.1, 3.1, 6.2:
            for kappa in 0.0, 2.3, 500.0:
                for _ in range(N):
                    sample = g.vonmisesvariate(mu, kappa)
                    self.assertTrue(
                        0 <= sample <= random.TWOPI,
                        msg=("vonmisesvariate({}, {}) produced a result {} out"
                             " of range [0, 2*pi]").format(mu, kappa, sample))

    def test_von_mises_large_kappa(self):
        # Issue #17141: vonmisesvariate() was hang for large kappas
        random.vonmisesvariate(0, 1e15)
        random.vonmisesvariate(0, 1e100)


class TestModule(unittest.TestCase):
    def testMagicConstants(self):
        self.assertAlmostEqual(random.NV_MAGICCONST, 1.71552776992141)
        self.assertAlmostEqual(random.TWOPI, 6.28318530718)
        self.assertAlmostEqual(random.LOG4, 1.38629436111989)
        self.assertAlmostEqual(random.SG_MAGICCONST, 2.50407739677627)

    def test__all__(self):
        # tests validity but not completeness of the __all__ list
        self.assertTrue(set(random.__all__) <= set(dir(random)))

    def test_random_subclass_with_kwargs(self):
        # SF bug #1486663 -- this used to erroneously raise a TypeError
        class Subclass(random.Random):
            def __init__(self, newarg=None):
                random.Random.__init__(self)
        Subclass(newarg=1)


def test_main(verbose=None):
    testclasses =    [WichmannHill_TestBasicOps,
                      MersenneTwister_TestBasicOps,
                      TestDistributions,
                      TestModule]

    try:
        random.SystemRandom().random()
    except NotImplementedError:
        pass
    else:
        testclasses.append(SystemRandom_TestBasicOps)

    test_support.run_unittest(*testclasses)

    # verify reference counting
    import sys
    if verbose and hasattr(sys, "gettotalrefcount"):
        counts = [None] * 5
        for i in xrange(len(counts)):
            test_support.run_unittest(*testclasses)
            counts[i] = sys.gettotalrefcount()
        print counts

if __name__ == "__main__":
    test_main(verbose=True)
