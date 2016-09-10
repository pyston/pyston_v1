# statcheck: ("-L" in EXTRA_JIT_ARGS) or (1 <= stats["num_osr_exits"] <= 2)

# While perhaps not required in practice, we should have the ability to
# OSR from inside a list comprehension.

def p(i):
    print i
print len([i for i in xrange(100000) if i % 100 == 0])
