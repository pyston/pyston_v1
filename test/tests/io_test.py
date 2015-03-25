import io
filename = "io_test_.txt"

f = io.open(filename, "w")
print type(f)
print f.isatty()
print f.write(unicode("Hello World"))
print f.tell()
f.close()

f = io.StringIO()
print type(f)
f.write(unicode('First line.\n'))
f.write(unicode('Second line.\n'))
print f.getvalue()
print f.isatty()
print f.tell()
f.close()

f = io.open(filename)
print type(f)
print f.read()
f.close()
