import fcntl

for k in sorted(dir(fcntl)):
    if k[0] == '_' or k != k.upper():
        continue
    print k, getattr(fcntl, k)

