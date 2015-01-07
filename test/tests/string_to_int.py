def test_string_to_int():
    cases = [
        ('0', 0),
        ('1', 1),
        ('9', 9),
        ('10', 10),
        ('09', 9),
        ('0000101', 101),    # not octal unless base 0 or 8
        ('5123', 5123),
        (' 0', 0),
        ('0  ', 0),
        (' \t \n   32313  \f  \v   \r  \n\r    ', 32313),
        ('+12', 12),
        ('-5', -5),
        ('- 5', -5),
        ('+ 5', 5),
        ('  -123456789 ', -123456789),
    ]

    for s, expected in cases:
        assert int(s) == expected

        print int(s)

def test_string_to_int_base():
    cases = [
        ('111', 2, 7),
        ('010', 2, 2),
        ('102', 3, 11),
        ('103', 4, 19),
        ('107', 8, 71),
        ('109', 10, 109),
        ('10A', 11, 131),
        ('10a', 11, 131),
        ('10f', 16, 271),
        ('10F', 16, 271),
        ('0x10f', 16, 271),
        ('0x10F', 16, 271),
        ('10z', 36, 1331),
        ('10Z', 36, 1331),
        ('12',   0, 12),
        ('015',  0, 13),
        ('0x10', 0, 16),
        ('0XE',  0, 14),
        ('0',    0, 0),
        ('0b11', 2, 3),
        ('0B10', 2, 2),
        ('0o77', 8, 63),
    ]

    for s, base, expected in cases:
        print 'int', int(s, base), '#', s, base
        print 'int with +', int('+'+s, base)
        print 'int with -', int('-'+s, base)
        print 'int with newline', int(s+'\n', base)
        print 'int with + and spaces', int('  +'+s, base)
        print 'int with - and spaces', int('-'+s+'  ', base)

        assert int(s, base) == expected
        assert int('+'+s, base) == expected
        assert int('-'+s, base) == -expected
        assert int(s+'\n', base) == expected
        assert int('  +'+s, base) == expected
        assert int('-'+s+'  ', base) == -expected


test_string_to_int()
test_string_to_int_base()