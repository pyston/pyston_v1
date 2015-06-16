import sre_compile

# This is the string tokenizer.PseudoToken:
pattern = '[ \\f\\t]*((\\\\\\r?\\n|\\Z|#[^\\r\\n]*|([uUbB]?[rR]?\'\'\'|[uUbB]?[rR]?"""))|((\\d+[jJ]|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)[jJ])|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)|(0[xX][\\da-fA-F]+[lL]?|0[bB][01]+[lL]?|(0[oO][0-7]+)|(0[0-7]*)[lL]?|[1-9]\\d*[lL]?))|((\\*\\*=?|>>=?|<<=?|<>|!=|//=?|[+\\-*/%&|^=<>]=?|~)|[][(){}]|(\\r?\\n|[:;.,`@]))|([uUbB]?[rR]?\'[^\\n\'\\\\]*(?:\\\\.[^\\n\'\\\\]*)*(\'|\\\\\\r?\\n)|[uUbB]?[rR]?"[^\\n"\\\\]*(?:\\\\.[^\\n"\\\\]*)*("|\\\\\\r?\\n))|[a-zA-Z_]\\w*)'

for i in xrange(200):
    # re.compile checks if the pattern is in the cache, and then calls sre_compile:
    sre_compile.compile(pattern, 0)
