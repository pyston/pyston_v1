import os.path

s = open(os.path.join(os.path.dirname(__file__), "coding_cp1252.py"), 'r').read()

exec s
