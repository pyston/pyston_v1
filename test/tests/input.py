import StringIO
import sys

success_tests = ["1234",            # just a number
                 "    123",         # whitespaces get trimmed
                 "str(5) + \"6\"",  # test for builtin function
                 "hasattr(__builtins__, 'get')"
                ]
special_tests = ["str(10)",
                 "hasattr(__builtins__, 'get')"
                 ]
failure_tests = ["abcd"]

orig_stdin = sys.stdin
sio = StringIO.StringIO("\n".join(success_tests + special_tests + failure_tests))
sys.stdin = sio

for _ in success_tests:
    print repr(input())

for _ in special_tests:
    # Special test: if the globals is empty, __builtin__ should be added to it
    # in the call to input().
    print repr(eval("input()", {}))

try:
    print repr(input())
except NameError:
    print "caught expected syntax error"

sys.stdin = orig_stdin
