import _ast
import struct
import sys
from types import NoneType

def _print_str(s, f):
    assert len(s) < 2**16
    f.write(struct.pack(">H", len(s)))
    f.write(s)

TYPE_MAP = {
        _ast.alias: 1,
        _ast.arguments: 2,
        _ast.Assert: 3,
        _ast.Assign: 4,
        _ast.Attribute: 5,
        _ast.AugAssign: 6,
        _ast.BinOp: 7,
        _ast.BoolOp: 8,
        _ast.Call: 9,
        _ast.ClassDef: 10,
        _ast.Compare: 11,
        _ast.comprehension: 12,
        _ast.Delete: 13,
        _ast.Dict: 14,
        _ast.Exec: 16,
        _ast.ExceptHandler: 17,
        _ast.ExtSlice: 18,
        _ast.Expr: 19,
        _ast.For: 20,
        _ast.FunctionDef: 21,
        _ast.GeneratorExp: 22,
        _ast.Global: 23,
        _ast.If: 24,
        _ast.IfExp: 25,
        _ast.Import: 26,
        _ast.ImportFrom: 27,
        _ast.Index: 28,
        _ast.keyword: 29,
        _ast.Lambda: 30,
        _ast.List: 31,
        _ast.ListComp: 32,
        _ast.Module: 33,
        _ast.Num: 34,
        _ast.Name: 35,
        _ast.Pass: 37,
        _ast.Pow: 38,
        _ast.Print: 39,
        _ast.Raise: 40,
        _ast.Repr: 41,
        _ast.Return: 42,
        _ast.Slice: 44,
        _ast.Str: 45,
        _ast.Subscript: 46,
        _ast.TryExcept: 47,
        _ast.TryFinally: 48,
        _ast.Tuple: 49,
        _ast.UnaryOp: 50,
        _ast.With: 51,
        _ast.While: 52,
        _ast.Yield: 53,

        _ast.Store: 54,
        _ast.Load: 55,
        _ast.Param: 56,
        _ast.Not: 57,
        _ast.In: 58,
        _ast.Is: 59,
        _ast.IsNot: 60,
        _ast.Or: 61,
        _ast.And: 62,
        _ast.Eq: 63,
        _ast.NotEq: 64,
        _ast.NotIn: 65,
        _ast.GtE: 66,
        _ast.Gt: 67,
        _ast.Mod: 68,
        _ast.Add: 69,
        _ast.Continue: 70,
        _ast.Lt: 71,
        _ast.LtE: 72,
        _ast.Break: 73,
        _ast.Sub: 74,
        _ast.Del: 75,
        _ast.Mult: 76,
        _ast.Div: 77,
        _ast.USub: 78,
        _ast.BitAnd: 79,
        _ast.BitOr: 80,
        _ast.BitXor: 81,
        _ast.RShift: 82,
        _ast.LShift: 83,
        _ast.Invert: 84,
        _ast.UAdd: 85,
        _ast.FloorDiv: 86,
    }

if sys.version_info >= (2,7):
    TYPE_MAP[_ast.DictComp] = 15
    TYPE_MAP[_ast.Set] = 43

def convert(n, f):
    assert n is None or isinstance(n, _ast.AST), repr(n)
    type_idx = TYPE_MAP[type(n)] if n else 0
    f.write(struct.pack(">B", type_idx))
    if n is None:
        return
    if isinstance(n, (_ast.operator, _ast.expr_context, _ast.boolop, _ast.cmpop, _ast.unaryop)):
        return

    f.write('\xae')

    if isinstance(n, _ast.Num):
        if isinstance(n.n, int):
            f.write('\x10')
        elif isinstance(n.n, float):
            f.write('\x20')
        else:
            raise Exception(type(n.n))

    # print >>sys.stderr, n, sorted(n.__dict__.items())
    for k, v in sorted(n.__dict__.items()):
        if k.startswith('_'):
            continue

        if k in ("vararg", "kwarg", "asname") and v is None:
            v = ""
        # elif k in ('col_offset', 'lineno'):
            # continue

        if isinstance(v, list):
            assert len(v) < 2**16
            f.write(struct.pack(">H", len(v)))
            if isinstance(n, _ast.Global):
                assert k == "names"
                for el in v:
                    _print_str(el, f)
            else:
                for el in v:
                    convert(el, f)
        elif isinstance(v, str):
            _print_str(v, f)
        elif isinstance(v, bool):
            f.write(struct.pack("B", v))
        elif isinstance(v, int):
            f.write(struct.pack(">q", v))
        elif isinstance(v, float):
            f.write(struct.pack(">d", v))
        elif v is None or isinstance(v, _ast.AST):
            convert(v, f)
        else:
            raise Exception((n, k, repr(v)))

if __name__ == "__main__":
    import time
    start = time.time()
    fn = sys.argv[1]
    s = open(fn).read()
    m = compile(s, fn, "exec", _ast.PyCF_ONLY_AST)

    convert(m, sys.stdout)

