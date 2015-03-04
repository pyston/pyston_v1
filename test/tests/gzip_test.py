# allow-warning: import level 0 will be treated as -1
import gzip
import io

text = "Hello World!"

# generate zip in memory
outdata = io.BytesIO()
timestamp = 1
f = gzip.GzipFile(None, "wb", 9, outdata, timestamp)
print f.write(text)
f.close()

output = outdata.getvalue()
print output.encode("base64")

# decode zip from memory
f = gzip.GzipFile(None, "rb", 9, io.BytesIO(output))
print f.read()
f.close()
