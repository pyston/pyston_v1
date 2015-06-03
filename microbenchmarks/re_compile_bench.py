import sre_compile
import sre_constants
import sre_parse
import re

IN = sre_constants.IN

def _identity(x):
    return x

for i in xrange(1000000):
    # sre_compile._optimize_charset([('negate', None), ('literal', 34), ('literal', 92)], _identity)
    # sre_compile._compile([17, 4, 0, 3, 0, 29, 12, 0, 4294967295L, 15, 7, 26, 19, 34, 19, 92, 0, 1, 28, 0, 0, 4294967295L, 7, 6, 19, 92, 2, 18, 15, 13, 19, 34, 5, 7, 0, 19, 34, 19, 34, 1, 18, 2, 0, 29, 0, 0, 4294967295L], [(IN, [('negate', None), ('literal', 34), ('literal', 92)])], 0)
    # sre_compile._compile([17, 8, 3, 1, 1, 1, 1, 97, 0], [('literal', 97)], 0)
    pass

# This is the string tokenizer.PseudoToken:
pattern = '[ \\f\\t]*((\\\\\\r?\\n|\\Z|#[^\\r\\n]*|([uUbB]?[rR]?\'\'\'|[uUbB]?[rR]?"""))|((\\d+[jJ]|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)[jJ])|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)|(0[xX][\\da-fA-F]+[lL]?|0[bB][01]+[lL]?|(0[oO][0-7]+)|(0[0-7]*)[lL]?|[1-9]\\d*[lL]?))|((\\*\\*=?|>>=?|<<=?|<>|!=|//=?|[+\\-*/%&|^=<>]=?|~)|[][(){}]|(\\r?\\n|[:;.,`@]))|([uUbB]?[rR]?\'[^\\n\'\\\\]*(?:\\\\.[^\\n\'\\\\]*)*(\'|\\\\\\r?\\n)|[uUbB]?[rR]?"[^\\n"\\\\]*(?:\\\\.[^\\n"\\\\]*)*("|\\\\\\r?\\n))|[a-zA-Z_]\\w*)'

for i in xrange(100):
    # re.compile checks if the pattern is in the cache, and then calls sre_compile:
    sre_compile.compile(pattern, 0)

for i in xrange(600):
    p = sre_parse.Pattern()
    p.flags = 0
    p.str = pattern
    sre_parse._parse_sub(sre_parse.Tokenizer(pattern), p, 0)
