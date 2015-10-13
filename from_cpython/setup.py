# CPython has a 2kloc version of this file

from distutils.core import setup, Extension
import glob
import os
import platform
import sysconfig

def relpath(fn):
    r =  os.path.join(os.path.dirname(__file__), fn)
    return r

def unique(f):
    # Use an array otherwise Python 2 gets confused about scoping.
    cache = []
    def wrapper(*args):
        if len(cache) == 0:
            cache.append(f(*args))
        return cache[0]
    return wrapper

@unique
def future_builtins_ext():
    return Extension("future_builtins", sources = map(relpath, [
        "Modules/future_builtins.c",
    ]))

@unique
def multiprocessing_ext():
    return Extension("_multiprocessing", sources = map(relpath, [
            "Modules/_multiprocessing/multiprocessing.c",
            "Modules/_multiprocessing/socket_connection.c",
            "Modules/_multiprocessing/semaphore.c",
            ]))

@unique
def bz2_ext():
    return Extension("bz2", sources = map(relpath, [
            "Modules/bz2module.c",
            ]), libraries = ['bz2'])

@unique
def ctypes_ext():
    ext = Extension("_ctypes", sources = map(relpath, [
            "Modules/_ctypes/_ctypes.c",
            "Modules/_ctypes/callbacks.c",
            "Modules/_ctypes/callproc.c",
            "Modules/_ctypes/stgdict.c",
            "Modules/_ctypes/cfield.c"
            ]))

    # Hack: Just copy the values of ffi_inc and ffi_lib from CPython's setup.py
    # May want something more robust later.
    ffi_inc = ['/usr/include/x86_64-linux-gnu']
    ffi_lib = "ffi_pic"

    if platform.linux_distribution()[0] == "Fedora":
        ffi_lib = "ffi"

    ext.include_dirs.extend(ffi_inc)
    ext.libraries.append(ffi_lib)

    return ext

@unique
def ctypes_test_ext():
    # TODO: I'm not sure how to use the ctypes tests, I just copied it over
    # from CPython's setup.py. For now we're only importing ctypes, not passing
    # all its tests.
    return Extension('_ctypes_test',
                     sources= map(relpath, ['Modules/_ctypes/_ctypes_test.c']))

@unique
def grp_ext():
    return Extension("grp", sources = map(relpath, [
            "Modules/grpmodule.c",
            ]))

@unique
def curses_ext():
    return Extension("_curses", sources = map(relpath, [
            "Modules/_cursesmodule.c",
            ]), libraries = ['curses'])

@unique
def readline_ext():
    return Extension("readline", sources = map(relpath, [
            "Modules/readline.c",
            ]))

@unique
def termios_ext():
    return Extension("termios", sources = map(relpath, [
            "Modules/termios.c",
            ]))

@unique
def mmap_ext():
    return Extension("mmap", sources = map(relpath, [
            "Modules/mmapmodule.c",
            ]))

@unique
def pyexpat_ext():
    define_macros = [('HAVE_EXPAT_CONFIG_H', '1'),]
    expat_sources = map(relpath, ['Modules/expat/xmlparse.c',
                                  'Modules/expat/xmlrole.c',
                                  'Modules/expat/xmltok.c',
                                  'Modules/pyexpat.c'])

    expat_depends = map(relpath, ['Modules/expat/ascii.h',
                                  'Modules/expat/asciitab.h',
                                  'Modules/expat/expat.h',
                                  'Modules/expat/expat_config.h',
                                  'Modules/expat/expat_external.h',
                                  'Modules/expat/internal.h',
                                  'Modules/expat/latin1tab.h',
                                  'Modules/expat/utf8tab.h',
                                  'Modules/expat/xmlrole.h',
                                  'Modules/expat/xmltok.h',
                                  'Modules/expat/xmltok_impl.h'
                                  ])
    return Extension('pyexpat',
                      define_macros = define_macros,
                      include_dirs = [relpath('Modules/expat')],
                      sources = expat_sources,
                      depends = expat_depends,
                    )

@unique
def elementtree_ext():
    # elementtree depends on expat
    pyexpat = pyexpat_ext()
    define_macros = pyexpat.define_macros + [('USE_PYEXPAT_CAPI', None),]
    return Extension('_elementtree',
                        define_macros = define_macros,
                        include_dirs = pyexpat.include_dirs,
                        libraries = pyexpat.libraries,
                        sources = [relpath('Modules/_elementtree.c')],
                        depends = pyexpat.depends,
                      )
ext_modules = [future_builtins_ext(),
               multiprocessing_ext(),
               pyexpat_ext(),
               elementtree_ext(),
               bz2_ext(),
               ctypes_ext(),
               ctypes_test_ext(),
               grp_ext(),
               curses_ext(),
               readline_ext(),
               termios_ext(),
               mmap_ext(),
               ]

builtin_headers = map(relpath, glob.glob("Include/*.h"))

for m in ext_modules:
    m.depends += builtin_headers


setup(name="Pyston", version="1.0", description="Pyston shared modules", ext_modules=ext_modules)
