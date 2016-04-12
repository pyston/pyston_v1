# TODO: move this back to intmethods.py once this is working
for b in range(26):
    try:
        print int('123', b)
    except ValueError as e:
        print e
    try:
        print int(u'123', b)
    except ValueError as e:
        print e

