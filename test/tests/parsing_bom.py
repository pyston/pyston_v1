# I really don't understand all the intricacies of unicode parsing, but apparently in addition to
# Python-specific coding lines, you can put a unicode byte order mark to signify that the text
# is encoded.
# http://en.wikipedia.org/wiki/Byte_order_mark
s = """\xef\xbb\xbfprint "hello world" """
exec s
