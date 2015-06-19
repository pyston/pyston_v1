
lines = [l for l in open("out.log").readlines() if l.startswith("malloc ") or l.startswith("free ")]

freed = set()
err = set()

for l in lines:
    if l.startswith("malloc"):
        p = l[7:]
        if p in freed:
            freed.remove(p)
    else:
        assert l.startswith("free")
        p = l[5:]
        if p.startswith("(nil)"):
            continue
        if p in freed:
            if p not in err:
                err.add(p)
                print p.strip()
        freed.add(p)

