f = open("/dev/null")
print repr(f.read())

f2 = file("/dev/null")
print repr(f2.read())

with open("/dev/null") as f3:
    print repr(f3.read())
