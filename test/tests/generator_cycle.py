# Test a generator cycle involving an unfinished generator.
import gc

def f():
    g = (i in (None, g) for i in xrange(2))
    print g.next()
print f()
gc.collect()
# print gc.garbage # XX pyston will put this into garbage currently

def f():
   g = None
   def G():
       c = g
       g
       yield 1
       yield 2
       yield 3
   g = G()
   g.next()

print f()
gc.collect()
# print gc.garbage # XX pyston will put this into garbage currently
