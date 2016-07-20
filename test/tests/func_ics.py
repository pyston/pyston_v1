# statcheck: stats['ic_attempts'] <= 2000
# statcheck: stats['ic_attempts_skipped_megamorphic'] == 0
ins = (1, 1.0, 1l, "", u"")
def f():
    for x in ins:
        isinstance(x, int)
        [].append(x)

for i in xrange(30000):
    f()
