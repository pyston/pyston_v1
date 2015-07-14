# CPython has a 2kloc version of this file

from distutils.core import setup, Extension
import os

def relpath(fn):
    r =  os.path.join(os.path.dirname(__file__), fn)
    return r

def multiprocessing_ext():
    return Extension("_multiprocessing", sources = map(relpath, [
            "Modules/_multiprocessing/multiprocessing.c",
            "Modules/_multiprocessing/socket_connection.c",
            "Modules/_multiprocessing/semaphore.c",
            ]))

def bz2_ext():
    return Extension("bz2", sources = map(relpath, [
            "Modules/bz2module.c",
            ]), libraries = ['bz2'])

def grp_ext():
    return Extension("grp", sources = map(relpath, [
            "Modules/grpmodule.c",
            ]))

def curses_ext():
    return Extension("_curses", sources = map(relpath, [
            "Modules/_cursesmodule.c",
            ]), libraries = ['curses'])

def termios_ext():
    return Extension("termios", sources = map(relpath, [
            "Modules/termios.c",
            ]))

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


setup(name="Pyston",
        version="1.0",
        description="Pyston shared modules",
        ext_modules=[multiprocessing_ext(), pyexpat_ext(), elementtree_ext(), bz2_ext(), grp_ext(), curses_ext(), termios_ext()]
    )
