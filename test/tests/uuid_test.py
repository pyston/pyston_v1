import uuid
print len(str(uuid.uuid1()))
print len(str(uuid.uuid4()))
print uuid.uuid3(uuid.NAMESPACE_DNS, 'python.org')
print uuid.uuid5(uuid.NAMESPACE_DNS, 'python.org')
x = uuid.UUID('{00010203-0405-0607-0809-0a0b0c0d0e0f}')
print str(x)
print uuid.UUID(bytes=x.bytes)
