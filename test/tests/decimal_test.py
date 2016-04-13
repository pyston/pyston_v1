from decimal import Decimal

for d in (Decimal("0.5"), Decimal("0"), Decimal(0), Decimal(1.0)):
    for f in [0, 1, 0.5, 0.0, 0.1]:
        print d, f,
        print d < f, d <= f, d > f, d >= f, d == f, d != f
