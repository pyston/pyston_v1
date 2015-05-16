# CPython has a 2kloc version of this file

from distutils.core import setup, Extension
import os

def relpath(fn):
    r =  os.path.join(os.path.dirname(__file__), fn)
    return r

setup(name="Pyston",
        version="1.0",
        description="Pyston shared modules",
        ext_modules=[Extension("_multiprocessing", sources = map(relpath, [
                "Modules/_multiprocessing/multiprocessing.c",
                "Modules/_multiprocessing/socket_connection.c",
                "Modules/_multiprocessing/semaphore.c",
            ]),
        )],
    )
