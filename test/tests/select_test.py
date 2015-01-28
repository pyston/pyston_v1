import select

for k in sorted(dir(select)):
    if not k.startswith("EPOLL") and not k.startswith("POLL"):
        continue
    print k, getattr(select, k)

