from __future__ import division

# compile() inherits the future flags of the parent module
print 1 / 2
exec "print 1 / 2"
exec compile("print 1 / 2", "<string>", "exec")
# But you can explicitly request that they not be inherited:
exec compile("print 1 / 2", "<string>", "exec", flags=0, dont_inherit=True)
exec compile("print 1 / 2", "<string>", "exec", flags=division.compiler_flag, dont_inherit=True)

# test the above but with eval
print eval(compile("1 / 2", "<string>", "eval"))
print eval(compile("1 / 2", "<string>", "eval", flags=0, dont_inherit=True))
print eval(compile("1 / 2", "<string>", "eval", flags=division.compiler_flag, dont_inherit=True))
