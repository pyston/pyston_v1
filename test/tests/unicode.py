# Unicode type
x = u'abc'
print type(x)
# print isinstance(x, basestring)

# Unicode representation (BMP).
# XXX In order to make this test pass, a encoding needs to be specified, but we don't support that yet.

# unicode_strings = [
#     u'κόσμε',
#     u'Вселена',
#     u'우주',
#     u'宇宙'
# ]
# for u in unicode_strings:
#     print repr(u)

# Single characters outside of the BMP.
# XXX unichr() not implemented yet.
# for ch in range(0x10000, 0x10ffff, 0xffab):
#     print unichr(ch)
