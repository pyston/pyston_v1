__builtins__.aoeu = 1
print aoeu

__builtins__.True = 2
print True
print bool(1)
print bool(1) is True

__builtins__.__builtins__ = 1
print __builtins__

__builtins__ = 2
print __builtins__

print all([]), all([True]), all([False]), all([None]), all([True, False, None])
print any([]), any([True]), any([False]), any([None]), any([True, False, None])
