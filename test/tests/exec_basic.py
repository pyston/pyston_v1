exec """print 'hi'
a = 5
print a"""

exec ""

try:
    exec ";"
    print "worked?"
except SyntaxError:
    pass

# Exec of a unicode encodes as utf8:
exec u"print repr('\u0180')"
print repr(eval(u"'\u0180'"))
