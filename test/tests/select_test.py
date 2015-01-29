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

