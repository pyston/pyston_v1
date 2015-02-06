import array
a = array.array("c", "hello world")
print type(a)
print a
a.append("!")
print a
for c in a:
    print c,
print a.tostring()
print a[2], a[4]
try:
    print a[200]
except IndexError:
    print "cought an IndexError"
