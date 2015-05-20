import time
print type(time)

time.sleep(0)
time.sleep(False)
time.clock()

print time.timezone

print long(time.mktime(time.localtime(1020)))

print time.sleep.__module__
