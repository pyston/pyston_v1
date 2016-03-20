print "-".join(["hello", "world"])
print "-".join(("hello", "world"))

print "hi".join("")
print "hi".join([])
print "hi".join(())

print repr(chr(0) + chr(180))
print repr('"')
# print repr("'") // don't feel like handling this right now; this should print out (verbatim) "'", ie realize it can use double quotes
print repr("'\"")

print str(object="test")

print "hello world\tmore\nwords\va\fb\ao".split()
print "  test  ".split()
print "  test  ".split(' ')
print "  test  ".split(None)
print "1<>2<>3".split('<>')
print "  test  ".rsplit()
print "  test  ".rsplit(' ')
print "  test  ".rsplit(None)
print "1<>2<>3".rsplit('<>')
print "1<>2<>3".rsplit('<>', 1)
print "1<>2<>3".split('<>', 1)

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
    print c, "hello world".count(c, 1, 2)
    print c, "hello world".count(c, 2, 5)

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

for i in xrange(-3, 5):
    print i, "bananananananananana".split("an", i)

for i in ["", "a", "ab", "aa"]:
    for j in ["", "b", "a", "ab", "aa"]:
        print i, j, i.startswith(j), j.startswith(i), i.endswith(j), j.endswith(i), i.find(j), j.find(i), i.rfind(j), j.rfind(i)

print "bananananananas".replace("anan", "an")
print "bananananananas".replace("anan", "An", 0)
print "bananananananas".replace("anan", "An", -1)
print "bananananananas".replace("anan", "An", 1)
print "bananananananas".replace("anan", "An", 5)

translation_map = [chr(c) for c in xrange(256)]
for c in "aeiou":
    translation_map[ord(c)] = c.upper()
translation_map = ''.join(translation_map)
print "hello world".translate(translation_map)
print "hello world".translate(translation_map, "")
print "hello world".translate(translation_map, "llo")
print "hello world".translate(None, "llo")

for i in xrange(-10, 10):
    print i, "aaaaa".find("a", i), "aaaa".find("a", 2, i)
    print i, "aaaaa".rfind("a", i), "aaaa".rfind("a", 2, i)

print "hello world".partition("hi")
print "hello world".partition("hello")
print "hello world".partition("o")

print "hello world"[False:True:True]

print "{hello}".format(hello="world")
print "%.3s" % "hello world"


for i in xrange(-5, 15):
    for j in xrange(-5, 15):
        print i, j, "banana".startswith("ana", i, j), "banana".endswith("ana", i, j)

def test_just_funcs(s, w):
    t1 = s.ljust(w, 'x')
    t2 = s.rjust(w, 'x')
    t3 = s.center(w, 'x')

    t4 = s.ljust(w)
    t5 = s.rjust(w)
    t6 = s.center(w)

    try:
        print s.ljust("a string")
    except TypeError as e:
        print e
    try:
        print s.rjust("a string")
    except TypeError:
        print e
    try:
        print s.center("a string")
    except TypeError:
        print e
    try:
        print s.ljust(10, 12345)
    except TypeError:
        print e
    try:
        print s.rjust(10, 12345)
    except TypeError:
        print e
    try:
        print s.center(10, 12345)
    except TypeError:
        print e

    print t1, t1 == s, t1 is s, type(t1)
    print t2, t2 == s, t2 is s, type(t2)
    print t3, t3 == s, t3 is s, type(t3)

    print t4, t4 == s, t4 is s, type(t4)
    print t5, t5 == s, t5 is s, type(t5)
    print t6, t6 == s, t6 is s, type(t6)

test_just_funcs("abcd", 3)
test_just_funcs("abcd", 4)
test_just_funcs("abcd", 5)
test_just_funcs("abcd", 6)
test_just_funcs("abcd", 7)
test_just_funcs("abcd", 8)
test_just_funcs("abcde", 3)
test_just_funcs("abcde", 4)
test_just_funcs("abcde", 5)
test_just_funcs("abcde", 6)
test_just_funcs("abcde", 7)
test_just_funcs("abcde", 8)

import gc
for c in "hello world":
    gc.collect()

print "hello world".index("world")
print "hello world".index("world", 1, 30)
print "hello world".index("l", 3)
print "hello world".rindex("world")
print "hello world".rindex("world", 1, 30)
print "hello world".rindex("l", 3)
try:
    print "hello world".index("goodbye")
except Exception as e:
    print e

print repr("hello\tworld\t".expandtabs())
print repr("hello\tworld\t".expandtabs(12))

print "hello world".startswith(("x", "h"))
print "hello world".endswith(("x", "h"))

print "partition:", "a.b.c.d".partition('.')
print "rpartition:", "a.b.c.d".rpartition('.')
print "partition:", "abcd".partition('.')
print "rpartition:", "abcd".rpartition('.')

print 'ab c\n\nde fg\rkl\r\n'.splitlines()
print 'ab c\n\nde fg\rkl\r\n'.splitlines(True)

print "1".zfill(3), "+1".zfill(3), "-1".zfill(3), "0".zfill(3)

it = iter("hello world")
print list(it)

print "'{0}' '{1}'".format("Hello " * 3, "Hello " * -3)

class C(object):
    def __str__(self):
        return "my class"
print "{0}".format(C())

def irgen_error():
	for i in range(1):
		fail = "test".format()
		print fail
		fail = "test {0} {1} {2}".format(1, 2, 3)
		print fail
irgen_error()

s = "hello"
for i in xrange(-8, 8):
    for j in xrange(-8, 8):
        print i,j, repr(s[i:j])
        for k in (-2, 1, 1, 2):
            print i,j,k, repr(s[i:j:k]), repr(s[slice(i, j, k)])
