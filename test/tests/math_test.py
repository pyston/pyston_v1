import math
print math.sqrt(2)
print math.sqrt(0)

print math.pi
print math.tan(math.pi)

print abs(1.0)
print abs(1)
print abs(-1)
print abs(-1.0)

print max(1, 2)
print min(1, 2)

print max(range(5))
print min(range(5))

for x in [float("inf"), math.pi]:
    print x, math.isinf(x), math.fabs(x), math.ceil(x), math.log(x), math.log10(x)

print math.sqrt.__name__
