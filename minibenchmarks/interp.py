
class Random(object):
    def __init__(self, seed):
        self.cur = seed

    def next(self):
        self.cur = (self.cur * 1103515245 + 12345) % (1 << 31)
        return self.cur

class AstAssign(object):
    def __init__(self, name, expr):
        self.name = name
        self.expr = expr

class AstWhile(object):
    def __init__(self, test, body):
        self.test = test
        self.body = body

class AstBinop(object):
    def __init__(self, lhs, rhs, op):
        self.lhs = lhs
        self.rhs = rhs
        self.op = op

class AstPrint(object):
    def __init__(self, expr):
        self.expr = expr

class AstIf(object):
    def __init__(self, test, then, orelse):
        self.test = test
        self.then = then
        self.orelse = orelse

class State(object):
    def __init__(self):
        self.syms = {}

def _run(state, node):
    if isinstance(node, list):
        for s in node:
            _run(state, s)
        return None
    elif isinstance(node, AstAssign):
        v = _run(state, node.expr)
        state.syms[node.name] = v
        return None
    elif isinstance(node, int):
        return node
    elif isinstance(node, str):
        return state.syms[node]
    elif isinstance(node, AstPrint):
        v = _run(state, node.expr)
        print v
    elif isinstance(node, AstBinop):
        l = _run(state, node.lhs)
        r = _run(state, node.rhs)
        if node.op == '+':
            return l + r
        if node.op == '-':
            return l - r
        if node.op == '*':
            return l * r
        if node.op == '%':
            return l % r
        if node.op == "<":
            return l < r
        if node.op == "<=":
            return l <= r
        if node.op == "==":
            return l == r
        print node.op
        1/0
    elif isinstance(node, AstIf):
        v = _run(state, node.test)
        if v:
            _run(state, node.then)
        else:
            _run(state, node.orelse)
    elif isinstance(node, AstWhile):
        while True:
            v = _run(state, node.test)
            if not v:
                break
            _run(state, node.body)
        return None
    else:
        print type(node)
        1/0

def run(node):
    _run(State(), node)

prog = [
        AstAssign("x", 100000),
        AstAssign("t", 0),
        AstWhile("x", [
            AstAssign("t", AstBinop("t", "x", '+')),
            AstAssign("x", AstBinop("x", 1, '-')),
            # AstPrint("x"),
            ]),
        AstPrint("t"),
        ]

prog = [
        AstAssign("i", 2),
        AstAssign("t", 0),
        AstWhile(AstBinop("i", 10000, '<'), [
            AstAssign("j", 2),
            AstAssign("good", 1),
            AstWhile(AstBinop(AstBinop("j", "j", '*'), "i", "<="), [
                AstIf(AstBinop(AstBinop("i", "j", "%"), 0, "=="), [
                    AstAssign("good", 0),
                ], [
                ]),
                AstAssign("j", AstBinop("j", 1, "+")),
            ]),
            AstIf("good", [
                AstPrint("i"),
                AstAssign("t", AstBinop("t", "i", "+")),
            ], []),
            AstAssign("i", AstBinop("i", 1, "+")),
        ]),
        AstPrint("t"),
    ]

run(prog)
