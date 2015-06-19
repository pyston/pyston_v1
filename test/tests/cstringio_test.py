import cStringIO
output = cStringIO.StringIO()
output.write("Hello\n")
print >>output, "World"
print output.getvalue()
output.close()
