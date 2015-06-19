import gc
import select

for k in sorted(dir(select)):
    if not k.startswith("EPOLL") and not k.startswith("POLL"):
        continue

    if k == "EPOLLET":
        v = select.EPOLLET
        if v == (-1 << 31):
            v = 1<<31
        print v
    else:
        print k, getattr(select, k)


p = select.poll()
f = open('/dev/null')
p.register(f)
gc.collect()
p.unregister(f)
p.register(f)
try:
    print len(p.poll(10))
finally:
    f.close()
gc.collect()
try:
    p.unregister(f)
except Exception:
    # We don't generate the write exception here, but we shouldn't abort
    pass
