import unicodedata
print unicodedata.lookup("EURO SIGN") == u"\u20ac"
print unicodedata.name(u"/")
print unicodedata.category(u"A")
