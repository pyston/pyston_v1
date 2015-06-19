import resource

for k in sorted(dir(resource)):
    if not k.startswith("RLIMIT_"):
        continue
    print k, getattr(resource, k)

TIME_LIMIT = 5
resource.setrlimit(resource.RLIMIT_CPU, (TIME_LIMIT + 1, TIME_LIMIT + 1))

MAX_MEM_MB = 100
resource.setrlimit(resource.RLIMIT_RSS, (MAX_MEM_MB * 1024 * 1024, MAX_MEM_MB * 1024 * 1024))
