# This is the string tokenizer.PseudoToken:
pattern = '[ \\f\\t]*((\\\\\\r?\\n|\\Z|#[^\\r\\n]*|([uUbB]?[rR]?\'\'\'|[uUbB]?[rR]?"""))|((\\d+[jJ]|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)[jJ])|((\\d+\\.\\d*|\\.\\d+)([eE][-+]?\\d+)?|\\d+[eE][-+]?\\d+)|(0[xX][\\da-fA-F]+[lL]?|0[bB][01]+[lL]?|(0[oO][0-7]+)|(0[0-7]*)[lL]?|[1-9]\\d*[lL]?))|((\\*\\*=?|>>=?|<<=?|<>|!=|//=?|[+\\-*/%&|^=<>]=?|~)|[][(){}]|(\\r?\\n|[:;.,`@]))|([uUbB]?[rR]?\'[^\\n\'\\\\]*(?:\\\\.[^\\n\'\\\\]*)*(\'|\\\\\\r?\\n)|[uUbB]?[rR]?"[^\\n"\\\\]*(?:\\\\.[^\\n"\\\\]*)*("|\\\\\\r?\\n))|[a-zA-Z_]\\w*)'

class Tokenizer:
    def __init__(self, string):
        self.string = string
        self.index = 0
        self.__next()
    def __next(self):
        if self.index >= len(self.string):
            self.next = None
            return
        char = self.string[self.index]
        if char[0] == "\\":
            try:
                c = self.string[self.index + 1]
            except IndexError:
                raise error, "bogus escape (end of line)"
            char = char + c
        self.index = self.index + len(char)
        self.next = char
    def match(self, char, skip=1):
        if char == self.next:
            if skip:
                self.__next()
            return 1
        return 0
    def get(self):
        this = self.next
        self.__next()
        return this
    def tell(self):
        return self.index, self.next
    def seek(self, index):
        self.index, self.next = index

for i in xrange(1000):
    t = Tokenizer(pattern)

    for j in xrange(500):
        t._Tokenizer__next()
        # n = t.get()
        # if not n:
            # break
    # sre_parse.parse(pattern, 0)

class C:
    pass

c = C()
c.a = 1
def f(c):
    n = 10000000
    while n:
        c.a
f(c)

