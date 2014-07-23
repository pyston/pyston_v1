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

testStr = " \t\n\v\ftest \t\n\v\f"
print repr(testStr.strip()), repr(testStr.lstrip()), repr(testStr.rstrip())
for c in [None, " ", "\t\f", "test"]:
    print repr(testStr.strip(c)), repr(testStr.lstrip(c)), repr(testStr.rstrip(c))


for pattern in ["hello", "o w", "nope"]:
    print pattern in "hello world"

print ord("\a")
for c in "hello world":
    print repr(c), ord(c)

for c in "hello world":
    print c, "hello world".count(c)

for i in xrange(1, 10):
    for j in xrange(1, 4):
        print ("a"*i).count("a"*j)

def test_comparisons(a, b):
    print "%s < %s = " % (a, b), a < b
    print "%s <= %s = " % (a, b), a <= b
    print "%s > %s = " % (a, b), a > b
    print "%s >= %s = " % (a, b), a >= b
    print "%s == %s = " % (a, b), a == b
    print "%s != %s = " % (a, b), a != b
test_comparisons("a", "a")
test_comparisons("a", "A")
test_comparisons("a", "aa")
test_comparisons("ab", "aa")

print sorted([str(i) for i in xrange(25)])
