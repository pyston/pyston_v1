print "-".join(["hello", "world"])

print repr(chr(0) + chr(180))
print repr('"')
# print repr("'") // don't feel like handling this right now; this should print out (verbatim) "'", ie realize it can use double quotes
print repr("'\"")

print "hello world\tmore\nwords\va\fb\ao".split()
print "  test  ".split()
print "  test  ".split(' ')
print "  test  ".split(None)
print "1<>2<>3".split('<>')
print "  test  ".rsplit()
print "  test  ".rsplit(' ')
print "  test  ".rsplit(None)
print "1<>2<>3".rsplit('<>')

print map(bool, ["hello", "", "world"])

if "":
    print "bad"

print repr(" \t\n\v\ftest \t\n\v\f".strip())

for pattern in ["hello", "o w", "nope"]:
    print pattern in "hello world"

print ord("\a")
for c in "hello world":
    print repr(c), ord(c)
