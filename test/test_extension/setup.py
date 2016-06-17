from distutils.core import setup, Extension
import glob
import os

extensions = [
    Extension("basic_test", sources = ["basic_test.c"]),
    Extension("descr_test", sources = ["descr_test.c"]),
    Extension("slots_test", sources = ["slots_test.c"]),
    Extension("type_test", sources = ["type_test.c"]),
    Extension("api_test", sources = ["api_test.c"]),
]

def relpath(fn):
    r =  os.path.join(os.path.dirname(__file__), fn)
    return r

builtin_headers = map(relpath, glob.glob("../../from_cpython/Include/*.h"))

for m in extensions:
    m.depends += builtin_headers


setup(name="test",
        version="1.0",
        description="test",
        ext_modules=extensions,
    )
