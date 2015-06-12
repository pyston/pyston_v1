# co is compiled without the __future__ division import.
# So even though the import is present in the exec statements,
# the code will be evaluated without it. Each should print 0.

co = compile("1 / 2", "<string>", "eval")

exec """
from __future__ import division
print eval(co)
"""

co = compile("print 1 / 2", "<string>", "exec")

exec """
from __future__ import division
exec co
"""
