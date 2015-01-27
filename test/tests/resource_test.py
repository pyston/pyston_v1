import resource

for k in sorted(dir(resource)):
    if not k.startswith("RLIMIT_"):
        continue
    print k, getattr(resource, k)
