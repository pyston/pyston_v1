def msg():
    print "msg()"
    return "failure message"

assert 1
assert True if "a" else 1, [msg() for i in xrange(5)]
assert 1, msg()
assert 0, msg()
