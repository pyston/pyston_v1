import slots_test

for i in xrange(3):
    t = slots_test.SlotsTester(i + 5)
    print t, repr(t), t()

print slots_test.SlotsTester.set_through_tpdict, slots_test.SlotsTester(5).set_through_tpdict
