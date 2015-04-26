from __future__ import division

print 1 / 2

exec """print 1 / 2"""

exec """
from __future__ import division
print 1 / 2
"""

print eval("1 / 2")

exec """
print eval("1 / 2")
"""

exec """
from __future__ import division
print eval("1 / 2")
"""
