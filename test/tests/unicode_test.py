# skip-if: '-x' in EXTRA_JIT_ARGS
# allow-warning: import level 0 will be treated as -1

print repr(unicode())
print repr(unicode('hello world'))

# Some random unicode character:
u = u'\u0180'
print len(u)
print repr(u)
print repr(u.encode("utf8"))

# This is tricky, since we need to support file encodings, and then set stdout to UTF8:
# print u
