import sre_parse

# This is the string tokenizer.PseudoToken:
pattern = '[ \\f\\t]*((\\\\\\r?\\n|\\Z|#[^\\r\\n]*|([uUbB]?[rR]?\'\'\'|[uUbB]?[rR]?"""))|((\\d+[jJ]|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)[jJ])|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)|(0[xX][\\da-fA-F]+[lL]?|0[bB][01]+[lL]?|(0[oO][0-7]+)|(0[0-7]*)[lL]?|[1-9]\\d*[lL]?))|((\\*\\*=?|>>=?|<<=?|<>|!=|//=?|[+\\-*/%&|^=<>]=?|~)|[][(){}]|(\\r?\\n|[:;.,`@]))|([uUbB]?[rR]?\'[^\\n\'\\\\]*(?:\\\\.[^\\n\'\\\\]*)*(\'|\\\\\\r?\\n)|[uUbB]?[rR]?"[^\\n"\\\\]*(?:\\\\.[^\\n"\\\\]*)*("|\\\\\\r?\\n))|[a-zA-Z_]\\w*)'

for i in xrange(600):
    p = sre_parse.Pattern()
    p.flags = 0
    p.str = pattern
    sre_parse._parse_sub(sre_parse.Tokenizer(pattern), p, 0)
