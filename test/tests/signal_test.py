import signal

for k in sorted(dir(signal)):
    if not k.startswith("SIG"):
        continue
    print k, getattr(signal, k)

print hasattr(signal, "alarm")
