from future_builtins import hex, oct, map, zip, filter
from itertools import imap, izip, ifilter

value_list1 = [0, 42, 42L, -42, -42L, "", (), {}, []]
value_list2 = [0, 100, 100L, -100, -100L]

for value1 in value_list1:
    try:
        print(hex(value1))
    except Exception as e:
        print(type(e), e.message)

for value2 in value_list2:
    try:
        print(oct(value2))
    except Exception as e:
        print(type(e), e.message)

print(map == imap)
print(zip == izip)
print(filter == ifilter)
