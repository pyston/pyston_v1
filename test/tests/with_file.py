f = open('/dev/null')
try:
    with f:
        1/0
except ZeroDivisionError as e:
    print e

try:
    f.readline()
except ValueError as e:
    print e
