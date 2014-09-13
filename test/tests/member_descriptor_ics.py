# run_args: -n
# statcheck: noninit_count('slowpath_member_descriptor_get') <= 50

f = open("/dev/null")

def go():
    # object
    s = slice(i, 1.0, "abc")
    print s.start
    print s.stop
    print s.step

    # byte
    # TODO 
    # using softspace works for this test as long as we implement softspace as
    # a member descriptor but it should actually a getset_descriptor
    # in addition to fixing that, we should replace it in this test with a legit
    # member descriptor
    print f.softspace

    # TODO figure out what uses BOOL and test that
    # TODO figure out what uses INT and test that

    # float
    c = 1j + i
    print c.real
    print c.imag

for i in xrange(1000):
    go();
