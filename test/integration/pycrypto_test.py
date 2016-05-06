import subprocess, sys, os, shutil, StringIO
sys.path.append(os.path.dirname(__file__) + "/../lib")
import test_helper

PATCHES = ["pycrypto_0001-fastmath-Add-support-for-Pyston.patch"]
PATCHES = [os.path.abspath(os.path.join(os.path.dirname(__file__), p)) for p in PATCHES]

pycrypto_dir = os.path.dirname(os.path.abspath(__file__)) + "/../lib/pycrypto"
os.chdir(pycrypto_dir)

for d in ("build", "install"):
    if os.path.exists(d):
         print "Removing the existing", d, "directory"
         shutil.rmtree(d)

devnull = open(os.devnull, "w")


print "-- Patching pycrypto"
for patch in PATCHES:
    try:
        cmd = ["patch", "-p1", "--forward", "-i", patch]
        subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
        print e.output
        if "Reversed (or previously applied) patch detected!  Skipping patch" not in e.output:
            raise e

print "-- Building pycrypto" 
subprocess.check_call([sys.executable, "setup.py", "build"], stdout=devnull)

print "-- Installing pycrypto" 
subprocess.check_call([sys.executable, "setup.py", "install", "--prefix=install"], stdout=devnull)

print "-- Testing pycrypto"
sys.path.append("install/site-packages")

test_string = "test string".ljust(16)

from Crypto.Hash import SHA256, MD5
assert SHA256.new(test_string).hexdigest() == "edce3184097ede907d91c4069c55104785a3a989b9706e5919202d6f5fe2d814"
assert MD5.new(test_string).hexdigest() == "e135865bb047e78e1827b0cf83696725"

from Crypto.Cipher import AES
aes1 = AES.new("pwd1__0123456789")
aes2 = AES.new("pwd2__0123456789")
enc_data = aes1.encrypt(test_string)
enc_data2 = aes2.encrypt(test_string)
assert enc_data != enc_data2
assert aes1.decrypt(enc_data) == test_string
assert aes2.decrypt(enc_data2) == test_string

from Crypto.PublicKey import RSA
from Crypto import Random
key = RSA.generate(1024, Random.new().read)
public_key = key.publickey()
enc_data = public_key.encrypt(test_string, 32)
assert enc_data != test_string
assert key.decrypt(enc_data) == test_string

expected = [{'ran': 1891}]
test_helper.run_test([sys.executable, "setup.py", "test"], pycrypto_dir, expected)

print "-- Tests finished"

print "-- Unpatching pycrypto"
for patch in reversed(PATCHES):
    cmd = ["patch", "-p1", "--forward", "-i", patch]
    cmd += ["-R"]
    subprocess.check_output(cmd, stderr=subprocess.STDOUT)

for d in ("build", "install"):
    if os.path.exists(d):
         print "Removing the created", d, "directory"
         shutil.rmtree(d)
