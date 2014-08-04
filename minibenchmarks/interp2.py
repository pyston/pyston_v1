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

def visit(visitor, node):
    if isinstance(node, list):
        for s in node:
            visit(visitor, s)
        return None

    type_name = type(node).__name__.lower()
    return getattr(visitor, "visit_" + type_name)(node)

class InterpVisitor(object):
    def __init__(self):
        self.syms = {}

    def visit(self, node):
        return visit(self, node)

    def visit_astassign(self, node):
        v = self.visit(node.expr)
        self.syms[node.name] = v

    def visit_int(self, node):
        return node

    def visit_str(self, node):
        return self.syms[node]

    def visit_astwhile(self, node):
        while True:
            v = self.visit(node.test)
            if not v:
                break
            self.visit(node.body)

    def visit_astbinop(self, node):
        l = self.visit(node.lhs)
        r = self.visit(node.rhs)
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

    def visit_astif(self, node):
        v = self.visit(node.test)
        if v:
            self.visit(node.then)
        else:
            self.visit(node.orelse)

    def visit_astprint(self, node):
        v = self.visit(node.expr)
        print v

def run(node):
    visit(InterpVisitor(), node)

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
        AstWhile(AstBinop("i", 5000, '<'), [
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
