# expected: fail
# - wip

import slots_test

for i in xrange(3):
    t = slots_test.SlotsTester(i + 5)
    print t, repr(t), t(), t[2]

print slots_test.SlotsTester.set_through_tpdict, slots_test.SlotsTester(5).set_through_tpdict

class C(object):
    def __repr__(self):
        print "__repr__()"
        return "<C object>"

slots_test.call_funcs(C())

# Test to make sure that updating an existing class also updates the tp_* slots:
def repr2(self):
    return "repr2()"
C.__repr__ = repr2
slots_test.call_funcs(C())
