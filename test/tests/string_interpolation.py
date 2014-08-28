print "%d" % 1
print "%02d" % 2
print "%f" % 1
print "%s" % 2

print "%(n)d %(x)f %(m)s" % {'x':1.0, 'n':2, 'm':"hello world"}
print "%(a(b))s" % {'a(b)': 1}

# I'm not sure if this is a feature or a bug, but both CPython and PyPy will accept it:
print "%s %(a)s" % {'a': 1}
