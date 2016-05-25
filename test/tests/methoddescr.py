# This code was legal in CPython 2.7.3 but became illegal in 2.7.4
# (there is a methoddescr.expected file for this test)

for i in xrange(1000):
    float.__dict__['fromhex'](float, "f0.04a")
