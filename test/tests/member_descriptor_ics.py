# skip-if: True
# this currently prints the wrong result in a release build

# run_args: -n
# statcheck: noninit_count('slowpath_member_descriptor_get') <= 1500

# Right now this will fail for the access with the try-block.
# All other accesses should be re-written.

import descr_test

def f(o, expect_exc):
    # print the repr so we make sure we get the types right (int or long)

    print 'short', repr(o.member_short)
    print 'int', repr(o.member_int)
    print 'long', repr(o.member_long)
    print 'float', repr(o.member_float)
    print 'double', repr(o.member_double)
    print 'string', repr(o.member_string)
    print 'string_inplace', repr(o.member_string_inplace)
    print 'char', repr(o.member_char)
    print 'byte', repr(o.member_byte)
    print 'ubyte', repr(o.member_ubyte)
    print 'ushort', repr(o.member_ushort)
    print 'uint', repr(o.member_uint)
    print 'ulong', repr(o.member_ulong)
    print 'bool', repr(o.member_bool)
    print 'object', repr(o.member_object)
    if expect_exc:
        try:
            o.member_object_ex
        except AttributeError as e:
            print 'object_ex got AttributeError', e.message
    else:
        print 'object_ex', repr(o.member_object_ex)
    print 'long_long', repr(o.member_long_long)
    print 'ulong_long', repr(o.member_ulong_long)
    print 'pyssizet', repr(o.member_pyssizet)

for i in xrange(1000):
    print "1:"
    f(descr_test.member_descr_object1, False)

    print "2:"
    f(descr_test.member_descr_object2, True)
