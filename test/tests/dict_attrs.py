# functools.partial has dict attrs
import functools

p = functools.partial(list)
p.__dict__ = dict(a=3)
print p.a

class P(functools.partial):
    pass
p = P(list)
p.__dict__ = dict(a=7)
