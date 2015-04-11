# expected: fail
import operator

for op in sorted(dir(operator)):
    if op.startswith("_"):
        continue
    print getattr(operator, op).__name__
