import os
import tempfile
import array

fd, fn = tempfile.mkstemp()

with open(fn, "wb") as f:
    f.write("hello world!\n")
    f.write(u"hello world2")
    f.write(array.array('c', 'hello world'))

with open(fn) as f:
    print repr(f.read())

with open(fn, "w") as f:
    f.write("hello world!")
    f.writelines(["hi", "world"])

with open(fn) as f:
    print repr(f.read())

with open(fn, "ab") as f:
    f.write("hello world!")

with open(fn) as f:
    print repr(f.read())

os.unlink(fn)
