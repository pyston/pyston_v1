names = ["%s.py" for i in xrange(100)]
import re
regex = re.compile(".*.py")
print type(regex)
for i in xrange(50000):
    for n in names:
        regex.match(n)
    # fnmatch.filter(names, "*.py")
