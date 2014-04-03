# Nested-loop test for OSR

y = 1000
t = 0
while y:
    x = y
    while x:
        x = x - 1
        t = t + x
    y = y - 1
print t
