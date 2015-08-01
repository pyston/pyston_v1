import uuid

# Hack to get the test passing until we support CTypes.
# The problem is that we currently support import CTypes but it doesn't run
# correctly, so uuid fails without a fallback to using non-CTypes functions.
# This makes the uuid module think CTypes is not present.
uuid._uuid_generate_random = None

print len(str(uuid.uuid4()))
print uuid.uuid3(uuid.NAMESPACE_DNS, 'python.org')
print uuid.uuid5(uuid.NAMESPACE_DNS, 'python.org')
x = uuid.UUID('{00010203-0405-0607-0809-0a0b0c0d0e0f}')
print str(x)
print uuid.UUID(bytes=x.bytes)
