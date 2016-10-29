# Autodetecting setup.py script for building the Python extensions
#

__version__ = "$Revision$"

import sys, os, imp, re, optparse
from glob import glob
from platform import machine as platform_machine
import sysconfig

from distutils.core import Extension, setup
from distutils.command.build_ext import build_ext
from distutils.command.install import install
from distutils.command.install_lib import install_lib
from distutils.spawn import find_executable

cross_compiling = "_PYTHON_HOST_PLATFORM" in os.environ

def get_platform():
    # cross build
    if "_PYTHON_HOST_PLATFORM" in os.environ:
        return os.environ["_PYTHON_HOST_PLATFORM"]
    # Get value of sys.platform
    if sys.platform.startswith('osf1'):
        return 'osf1'
    return sys.platform
host_platform = get_platform()


def add_dir_to_list(dirlist, dir):
    """Add the directory 'dir' to the list 'dirlist' (at the front) if
    1) 'dir' is not already in 'dirlist'
    2) 'dir' actually exists, and is a directory."""
    if dir is not None and os.path.isdir(dir) and dir not in dirlist:
        dirlist.insert(0, dir)

def macosx_sdk_root():
    """
    Return the directory of the current OSX SDK,
    or '/' if no SDK was specified.
    """
    cflags = sysconfig.get_config_var('CFLAGS')
    m = re.search(r'-isysroot\s+(\S+)', cflags)
    if m is None:
        sysroot = '/'
    else:
        sysroot = m.group(1)
    return sysroot

def is_macosx_sdk_path(path):
    """
    Returns True if 'path' can be located in an OSX SDK
    """
    return ( (path.startswith('/usr/') and not path.startswith('/usr/local'))
                or path.startswith('/System/')
                or path.startswith('/Library/') )

def find_file(filename, std_dirs, paths):
    """Searches for the directory where a given file is located,
    and returns a possibly-empty list of additional directories, or None
    if the file couldn't be found at all.

    'filename' is the name of a file, such as readline.h or libcrypto.a.
    'std_dirs' is the list of standard system directories; if the
        file is found in one of them, no additional directives are needed.
    'paths' is a list of additional locations to check; if the file is
        found in one of them, the resulting list will contain the directory.
    """
    if host_platform == 'darwin':
        # Honor the MacOSX SDK setting when one was specified.
        # An SDK is a directory with the same structure as a real
        # system, but with only header files and libraries.
        sysroot = macosx_sdk_root()

    # Check the standard locations
    for dir in std_dirs:
        f = os.path.join(dir, filename)

        if host_platform == 'darwin' and is_macosx_sdk_path(dir):
            f = os.path.join(sysroot, dir[1:], filename)

        if os.path.exists(f): return []

    # Check the additional directories
    for dir in paths:
        f = os.path.join(dir, filename)

        if host_platform == 'darwin' and is_macosx_sdk_path(dir):
            f = os.path.join(sysroot, dir[1:], filename)

        if os.path.exists(f):
            return [dir]

    # Not found anywhere
    return None

def find_library_file(compiler, libname, std_dirs, paths):
    result = compiler.find_library_file(std_dirs + paths, libname)
    if result is None:
        return None

    if host_platform == 'darwin':
        sysroot = macosx_sdk_root()

    # Check whether the found file is in one of the standard directories
    dirname = os.path.dirname(result)
    for p in std_dirs:
        # Ensure path doesn't end with path separator
        p = p.rstrip(os.sep)

        if host_platform == 'darwin' and is_macosx_sdk_path(p):
            if os.path.join(sysroot, p[1:]) == dirname:
                return [ ]

        if p == dirname:
            return [ ]

    # Otherwise, it must have been in one of the additional directories,
    # so we have to figure out which one.
    for p in paths:
        # Ensure path doesn't end with path separator
        p = p.rstrip(os.sep)

        if host_platform == 'darwin' and is_macosx_sdk_path(p):
            if os.path.join(sysroot, p[1:]) == dirname:
                return [ p ]

        if p == dirname:
            return [p]
    else:
        assert False, "Internal error: Path not found in std_dirs or paths"

def module_enabled(extlist, modname):
    """Returns whether the module 'modname' is present in the list
    of extensions 'extlist'."""
    extlist = [ext for ext in extlist if ext.name == modname]
    return len(extlist)

def find_module_file(module, dirlist):
    """Find a module in a set of possible folders. If it is not found
    return the unadorned filename"""
    list = find_file(module, [], dirlist)
    if not list:
        return module
    if len(list) > 1:
        log.info("WARNING: multiple copies of %s found"%module)
    return os.path.join(list[0], module)

# Pyston change: find the real path of source file
def relpath(fn):
    r =  os.path.join(os.path.dirname(__file__), fn)
    return r


# This global variable is used to hold the list of modules to be disabled.
disabled_module_list = []

class PyBuildExt(build_ext):

    def __init__(self, dist):
        build_ext.__init__(self, dist)
        self.failed = []

    def build_extensions(self):

        # Detect which modules should be compiled
        missing = self.detect_modules()

        # Remove modules that are present on the disabled list
        extensions = [ext for ext in self.extensions
                      if ext.name not in disabled_module_list]
        # move ctypes to the end, it depends on other modules
        ext_map = dict((ext.name, i) for i, ext in enumerate(extensions))
        if "_ctypes" in ext_map:
            ctypes = extensions.pop(ext_map["_ctypes"])
            extensions.append(ctypes)
        self.extensions = extensions

        # Fix up the autodetected modules, prefixing all the source files
        # with Modules/ and adding Python's include directory to the path.
        (srcdir,) = sysconfig.get_config_vars('srcdir')
        if not srcdir:
            # Maybe running on Windows but not using CYGWIN?
            raise ValueError("No source directory; cannot proceed.")
        srcdir = os.path.abspath(srcdir)
        moddirlist = [os.path.join(srcdir, '../from_cpython/Modules')]

        # Platform-dependent module source and include directories
        incdirlist = []

        if host_platform == 'darwin' and ("--disable-toolbox-glue" not in
            sysconfig.get_config_var("CONFIG_ARGS")):
            # Mac OS X also includes some mac-specific modules
            macmoddir = os.path.join(srcdir, 'Mac/Modules')
            moddirlist.append(macmoddir)
            incdirlist.append(os.path.join(srcdir, 'Mac/Include'))

        # Pyston change: Pyston doesn't use this for now.
        # Fix up the paths for scripts, too
        # self.distribution.scripts = [os.path.join(srcdir, filename)
        #                              for filename in self.distribution.scripts]

        # Python header files
        headers = [sysconfig.get_config_h_filename()]
        headers += glob(os.path.join(sysconfig.get_path('include'), "*.h"))
        for ext in self.extensions[:]:
            ext.sources = [ find_module_file(filename, moddirlist)
                            for filename in ext.sources ]
            if ext.depends is not None:
                ext.depends = [find_module_file(filename, moddirlist)
                               for filename in ext.depends]
            else:
                ext.depends = []
            # re-compile extensions if a header file has been changed
            ext.depends.extend(headers)

            # platform specific include directories
            ext.include_dirs.extend(incdirlist)

            # If a module has already been built statically,
            # don't build it here
            if ext.name in sys.builtin_module_names:
                self.extensions.remove(ext)

        # Pyston change: comment this out, we don't have this file, so didn't
        # use this block of code
        # # Parse Modules/Setup and Modules/Setup.local to figure out which
        # # modules are turned on in the file.
        # remove_modules = []
        # for filename in ('Modules/Setup', 'Modules/Setup.local'):
        #     input = text_file.TextFile(filename, join_lines=1)
        #     while 1:
        #         line = input.readline()
        #         if not line: break
        #         line = line.split()
        #         remove_modules.append(line[0])
        #     input.close()
        #
        # for ext in self.extensions[:]:
        #     if ext.name in remove_modules:
        #         self.extensions.remove(ext)
        #
        # # When you run "make CC=altcc" or something similar, you really want
        # # those environment variables passed into the setup.py phase.  Here's
        # # a small set of useful ones.
        # compiler = os.environ.get('CC')
        # args = {}
        # # unfortunately, distutils doesn't let us provide separate C and C++
        # # compilers
        # if compiler is not None:
        #     (ccshared,cflags) = sysconfig.get_config_vars('CCSHARED','CFLAGS')
        #     args['compiler_so'] = compiler + ' ' + ccshared + ' ' + cflags
        # self.compiler.set_executables(**args)
        build_ext.build_extensions(self)

        longest = max([len(e.name) for e in self.extensions])
        if self.failed:
            longest = max(longest, max([len(name) for name in self.failed]))

        def print_three_column(lst):
            lst.sort(key=str.lower)
            # guarantee zip() doesn't drop anything
            while len(lst) % 3:
                lst.append("")
            for e, f, g in zip(lst[::3], lst[1::3], lst[2::3]):
                print "%-*s   %-*s   %-*s" % (longest, e, longest, f,
                                              longest, g)

        if missing:
            print
            print ("Python build finished, but the necessary bits to build "
                   "these modules were not found:")
            print_three_column(missing)
            print ("To find the necessary bits, look in setup.py in"
                   " detect_modules() for the module's name.")
            print

        if self.failed:
            failed = self.failed[:]
            print
            print "Failed to build these modules:"
            print_three_column(failed)
            print

    def build_extension(self, ext):

        if ext.name == '_ctypes':
            if not self.configure_ctypes(ext):
                return

        try:
            build_ext.build_extension(self, ext)
        except (CCompilerError, DistutilsError), why:
            self.announce('WARNING: building of extension "%s" failed: %s' %
                          (ext.name, sys.exc_info()[1]))
            self.failed.append(ext.name)
            return
        # Workaround for Mac OS X: The Carbon-based modules cannot be
        # reliably imported into a command-line Python
        if 'Carbon' in ext.extra_link_args:
            self.announce(
                'WARNING: skipping import check for Carbon-based "%s"' %
                ext.name)
            return

        if host_platform == 'darwin' and (
                sys.maxint > 2**32 and '-arch' in ext.extra_link_args):
            # Don't bother doing an import check when an extension was
            # build with an explicit '-arch' flag on OSX. That's currently
            # only used to build 32-bit only extensions in a 4-way
            # universal build and loading 32-bit code into a 64-bit
            # process will fail.
            self.announce(
                'WARNING: skipping import check for "%s"' %
                ext.name)
            return

        # Workaround for Cygwin: Cygwin currently has fork issues when many
        # modules have been imported
        if host_platform == 'cygwin':
            self.announce('WARNING: skipping import check for Cygwin-based "%s"'
                % ext.name)
            return
        ext_filename = os.path.join(
            self.build_lib,
            self.get_ext_filename(self.get_ext_fullname(ext.name)))

        # Don't try to load extensions for cross builds
        if cross_compiling:
            return

        try:
            imp.load_dynamic(ext.name, ext_filename)
        except ImportError, why:
            self.failed.append(ext.name)
            self.announce('*** WARNING: renaming "%s" since importing it'
                          ' failed: %s' % (ext.name, why), level=3)
            assert not self.inplace
            basename, tail = os.path.splitext(ext_filename)
            newname = basename + "_failed" + tail
            if os.path.exists(newname):
                os.remove(newname)
            os.rename(ext_filename, newname)

            # XXX -- This relies on a Vile HACK in
            # distutils.command.build_ext.build_extension().  The
            # _built_objects attribute is stored there strictly for
            # use here.
            # If there is a failure, _built_objects may not be there,
            # so catch the AttributeError and move on.
            try:
                for filename in self._built_objects:
                    os.remove(filename)
            except AttributeError:
                self.announce('unable to remove files (ignored)')
        except:
            exc_type, why, tb = sys.exc_info()
            self.announce('*** WARNING: importing extension "%s" '
                          'failed with %s: %s' % (ext.name, exc_type, why),
                          level=3)
            self.failed.append(ext.name)

    def add_multiarch_paths(self):
        # Debian/Ubuntu multiarch support.
        # https://wiki.ubuntu.com/MultiarchSpec
        cc = sysconfig.get_config_var('CC')
        tmpfile = os.path.join(self.build_temp, 'multiarch')
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
        ret = os.system(
            '%s -print-multiarch > %s 2> /dev/null' % (cc, tmpfile))
        multiarch_path_component = ''
        try:
            if ret >> 8 == 0:
                with open(tmpfile) as fp:
                    multiarch_path_component = fp.readline().strip()
        finally:
            os.unlink(tmpfile)

        if multiarch_path_component != '':
            add_dir_to_list(self.compiler.library_dirs,
                            '/usr/lib/' + multiarch_path_component)
            add_dir_to_list(self.compiler.include_dirs,
                            '/usr/include/' + multiarch_path_component)
            return

        if not find_executable('dpkg-architecture'):
            return
        opt = ''
        if cross_compiling:
            opt = '-t' + sysconfig.get_config_var('HOST_GNU_TYPE')
        tmpfile = os.path.join(self.build_temp, 'multiarch')
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
        ret = os.system(
            'dpkg-architecture %s -qDEB_HOST_MULTIARCH > %s 2> /dev/null' %
            (opt, tmpfile))
        try:
            if ret >> 8 == 0:
                with open(tmpfile) as fp:
                    multiarch_path_component = fp.readline().strip()
                add_dir_to_list(self.compiler.library_dirs,
                                '/usr/lib/' + multiarch_path_component)
                add_dir_to_list(self.compiler.include_dirs,
                                '/usr/include/' + multiarch_path_component)
        finally:
            os.unlink(tmpfile)

    def add_gcc_paths(self):
        gcc = sysconfig.get_config_var('CC')
        tmpfile = os.path.join(self.build_temp, 'gccpaths')
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
        ret = os.system('%s -E -v - </dev/null 2>%s 1>/dev/null' % (gcc, tmpfile))
        is_gcc = False
        in_incdirs = False
        inc_dirs = []
        lib_dirs = []
        try:
            if ret >> 8 == 0:
                with open(tmpfile) as fp:
                    for line in fp.readlines():
                        if line.startswith("gcc version"):
                            is_gcc = True
                        elif line.startswith("#include <...>"):
                            in_incdirs = True
                        elif line.startswith("End of search list"):
                            in_incdirs = False
                        elif is_gcc and line.startswith("LIBRARY_PATH"):
                            for d in line.strip().split("=")[1].split(":"):
                                d = os.path.normpath(d)
                                if '/gcc/' not in d:
                                    add_dir_to_list(self.compiler.library_dirs,
                                                    d)
                        elif is_gcc and in_incdirs and '/gcc/' not in line:
                            add_dir_to_list(self.compiler.include_dirs,
                                            line.strip())
        finally:
            os.unlink(tmpfile)

    def detect_modules(self):
        # Ensure that /usr/local is always used
        if not cross_compiling:
            add_dir_to_list(self.compiler.library_dirs, '/usr/local/lib')
            add_dir_to_list(self.compiler.include_dirs, '/usr/local/include')
        # Pyston change: we don't support cross compiling yet
        # if cross_compiling:
        #     self.add_gcc_paths()
        # self.add_multiarch_paths()

        # Add paths specified in the environment variables LDFLAGS and
        # CPPFLAGS for header and library files.
        # We must get the values from the Makefile and not the environment
        # directly since an inconsistently reproducible issue comes up where
        # the environment variable is not set even though the value were passed
        # into configure and stored in the Makefile (issue found on OS X 10.3).
        for env_var, arg_name, dir_list in (
                ('LDFLAGS', '-R', self.compiler.runtime_library_dirs),
                ('LDFLAGS', '-L', self.compiler.library_dirs),
                ('CPPFLAGS', '-I', self.compiler.include_dirs)):
            env_val = sysconfig.get_config_var(env_var)
            if env_val:
                # To prevent optparse from raising an exception about any
                # options in env_val that it doesn't know about we strip out
                # all double dashes and any dashes followed by a character
                # that is not for the option we are dealing with.
                #
                # Please note that order of the regex is important!  We must
                # strip out double-dashes first so that we don't end up with
                # substituting "--Long" to "-Long" and thus lead to "ong" being
                # used for a library directory.
                env_val = re.sub(r'(^|\s+)-(-|(?!%s))' % arg_name[1],
                                 ' ', env_val)
                parser = optparse.OptionParser()
                # Make sure that allowing args interspersed with options is
                # allowed
                parser.allow_interspersed_args = True
                parser.error = lambda msg: None
                parser.add_option(arg_name, dest="dirs", action="append")
                options = parser.parse_args(env_val.split())[0]
                if options.dirs:
                    for directory in reversed(options.dirs):
                        add_dir_to_list(dir_list, directory)

        if os.path.normpath(sys.prefix) != '/usr' \
                and not sysconfig.get_config_var('PYTHONFRAMEWORK'):
            # OSX note: Don't add LIBDIR and INCLUDEDIR to building a framework
            # (PYTHONFRAMEWORK is set) to avoid # linking problems when
            # building a framework with different architectures than
            # the one that is currently installed (issue #7473)
            add_dir_to_list(self.compiler.library_dirs,
                            sysconfig.get_config_var("LIBDIR"))
            add_dir_to_list(self.compiler.include_dirs,
                            sysconfig.get_config_var("INCLUDEDIR"))

        try:
            have_unicode = unicode
        except NameError:
            have_unicode = 0

        # lib_dirs and inc_dirs are used to search for files;
        # if a file is found in one of those directories, it can
        # be assumed that no additional -I,-L directives are needed.
        inc_dirs = self.compiler.include_dirs[:]
        lib_dirs = self.compiler.library_dirs[:]
        if not cross_compiling:
            for d in (
                '/usr/include',
                ):
                add_dir_to_list(inc_dirs, d)
            for d in (
                '/lib64', '/usr/lib64',
                # Pyston change: add additional search path for lib
                '/lib', '/usr/lib', '/usr/lib/x86_64-linux-gnu'
                ):
                add_dir_to_list(lib_dirs, d)
        exts = []
        missing = []

        config_h = sysconfig.get_config_h_filename()
        config_h_vars = sysconfig.parse_config_h(open(config_h))

        srcdir = sysconfig.get_config_var('srcdir')

        # Check for AtheOS which has libraries in non-standard locations
        if host_platform == 'atheos':
            lib_dirs += ['/system/libs', '/atheos/autolnk/lib']
            lib_dirs += os.getenv('LIBRARY_PATH', '').split(os.pathsep)
            inc_dirs += ['/system/include', '/atheos/autolnk/include']
            inc_dirs += os.getenv('C_INCLUDE_PATH', '').split(os.pathsep)

        # OSF/1 and Unixware have some stuff in /usr/ccs/lib (like -ldb)
        if host_platform in ['osf1', 'unixware7', 'openunix8']:
            lib_dirs += ['/usr/ccs/lib']

        # HP-UX11iv3 keeps files in lib/hpux folders.
        if host_platform == 'hp-ux11':
            lib_dirs += ['/usr/lib/hpux64', '/usr/lib/hpux32']

        if host_platform == 'darwin':
            # This should work on any unixy platform ;-)
            # If the user has bothered specifying additional -I and -L flags
            # in OPT and LDFLAGS we might as well use them here.
            #   NOTE: using shlex.split would technically be more correct, but
            # also gives a bootstrap problem. Let's hope nobody uses directories
            # with whitespace in the name to store libraries.
            cflags, ldflags = sysconfig.get_config_vars(
                    'CFLAGS', 'LDFLAGS')
            for item in cflags.split():
                if item.startswith('-I'):
                    inc_dirs.append(item[2:])

            for item in ldflags.split():
                if item.startswith('-L'):
                    lib_dirs.append(item[2:])

        # Check for MacOS X, which doesn't need libm.a at all
        math_libs = ['m']
        if host_platform in ['darwin', 'beos']:
            math_libs = []

        # XXX Omitted modules: gl, pure, dl, SGI-specific modules

        #
        # The following modules are all pretty straightforward, and compile
        # on pretty much any POSIXish platform.
        #

        # Pyston change: the _weakref module is handled by from_cpython/CMakefile.txt
        # Some modules that are normally always on:
        #exts.append( Extension('_weakref', ['_weakref.c']) )

        # array objects
        # Pyston change: the array module is handled by from_cpython/CMakefile
        # exts.append( Extension('array', ['arraymodule.c']) )
        # complex math library functions
        exts.append( Extension('cmath', sources = map(relpath, [
            "Modules/cmathmodule.c",]),
            depends=['_math.h'],
        ))
        # math library functions, e.g. sin()
        exts.append( Extension('math',  ['mathmodule.c', '_math.c'],
                               depends=['_math.h'],
                               libraries=math_libs) )
        # Pyston change: the modules list in this section are handled by from_cpython/CMakefile.txt
        # # fast string operations implemented in C
        # exts.append( Extension('strop', ['stropmodule.c']) )
        # # time operations and variables
        # exts.append( Extension('time', ['timemodule.c'],
        #                        libraries=math_libs) )
        # # Pyston change: the datetime handled by from_cpython/CMakefile.txt
        # exts.append( Extension('datetime', ['datetimemodule.c', 'timemodule.c'],
        #                        libraries=math_libs) )
        # # Pyston change: comment out, handled by from_cpython/CMakefile.txt
        # # fast iterator tools implemented in C
        # exts.append( Extension("itertools", ["itertoolsmodule.c"]) )
        # code that will be builtins in the future, but conflict with the
        #  current builtins
        exts.append( Extension('future_builtins',  sources = map(relpath, [
            "Modules/future_builtins.c",
        ])) )

        # Pyston change: comment out, those modules are handled by from_cpython/CMakefile.txt
        # # random number generator implemented in C
        # exts.append( Extension("_random", ["_randommodule.c"]) )
        # # Pyston change: handled by from_cpython/CMakefile.txt
        # # high-performance collections
        # exts.append( Extension("_collections", ["_collectionsmodule.c"]) )
        # Pyston change: the bisect module handled by from_cpython/CMakefile.txt
        # # bisect
        # exts.append( Extension("_bisect", ["_bisectmodule.c"]) )
        # Pyston change: the _heapq module handled by from_cpython/CMakefile.txt
        # # heapq
        # exts.append( Extension("_heapq", ["_heapqmodule.c"]) )

        # Pyston change: the operator module handled by from_cpython/CMakefile
        # # operator.add() and similar goodies
        # exts.append( Extension('operator', ['operator.c']) )

        # Pyston change: io module handled by from_cpython/CMakefile.txt
        # # Python 3.1 _io library
        # exts.append( Extension("_io",
        #                        ["_io/bufferedio.c", "_io/bytesio.c", "_io/fileio.c",
        #                         "_io/iobase.c", "_io/_iomodule.c", "_io/stringio.c", "_io/textio.c"],
        #                        depends=["_io/_iomodule.h"], include_dirs=["Modules/_io"]))
        # Pyston change: handled by from_cpython/CMakefile.txt
        # # _functools
        # exts.append( Extension("_functools", ["_functoolsmodule.c"]) )
        # # _json speedups
        # exts.append( Extension("_json", ["_json.c"]) )
        # # Python C API test module
        # exts.append( Extension('_testcapi', ['_testcapimodule.c'],
        #                        depends=['testcapi_long.h']) )
        # # profilers (_lsprof is for cProfile.py)
        # exts.append( Extension('_hotshot', ['_hotshot.c']) )
        # exts.append( Extension('_lsprof', ['_lsprof.c', 'rotatingtree.c']) )
        # Pyston change: the unicodedata module handled by from_cpython/CMakefile.txt
        # static Unicode character database
        # if have_unicode:
        #     exts.append( Extension('unicodedata', ['unicodedata.c']) )
        # else:
        #     missing.append('unicodedata')

        # access to ISO C locale support
        # data = open('pyconfig.h').read()
        # m = re.search(r"#s*define\s+WITH_LIBINTL\s+1\s*", data)
        # Pyston change: hard code the variable for now
        m = None
        if m is not None:
            locale_libs = ['intl']
        else:
            locale_libs = []
        if host_platform == 'darwin':
            locale_extra_link_args = ['-framework', 'CoreFoundation']
        else:
            locale_extra_link_args = []

        exts.append( Extension('_locale', sources = map(relpath, [ 
            "Modules/_localemodule.c",
        ])) )

        # Modules with some UNIX dependencies -- on by default:
        # (If you have a really backward UNIX, select and socket may not be
        # supported...)

        # fcntl(2) and ioctl(2)
        # Pyston change: handled by from_cpython/CMakefile.txt
        # libs = []
        # if (config_h_vars.get('FLOCK_NEEDS_LIBBSD', False)):
        #     # May be necessary on AIX for flock function
        #     libs = ['bsd']
        # exts.append( Extension('fcntl', ['fcntlmodule.c'], libraries=libs) )
        # pwd(3)
        # Pyston change: the pwd module handled by from_cpython/CMakefile.txt
        # exts.append( Extension('pwd', ['pwdmodule.c']) )
        # grp(3)
        exts.append( Extension('grp', sources = map(relpath, [
            "Modules/grpmodule.c",
        ])) )
        # spwd, shadow passwords
        # Pyston change: handled by from_cpython/CMakefile.txt
        # if (config_h_vars.get('HAVE_GETSPNAM', False) or
        #         config_h_vars.get('HAVE_GETSPENT', False)):
        #     exts.append( Extension('spwd', ['spwdmodule.c']) )
        # else:
        #     missing.append('spwd')

        # Pyston change: the select module handled by from_cpython/CMakefile.txt
        # select(2); not on ancient System V
        # exts.append( Extension('select', ['selectmodule.c']) )

        # Fred Drake's interface to the Python parser
        exts.append( Extension('parser',  sources = map(relpath, [
            "Modules/parsermodule.c",
        ])) )

        # cStringIO and cPickle
        exts.append( Extension('cStringIO', sources = map(relpath, [
            "Modules/cStringIO.c",
        ])) )
        exts.append( Extension('cPickle', sources = map(relpath, [
            "Modules/cPickle.c",
        ])) )

        # Memory-mapped files (also works on Win32).
        if host_platform not in ['atheos']:
            exts.append( Extension('mmap', sources = map(relpath, [
                "Modules/mmapmodule.c",
            ])) )
        else:
            missing.append('mmap')

        # Lance Ellinghaus's syslog module
        # syslog daemon interface
        # Pyston change: disable the those modules in this setup.py
        # exts.append( Extension('syslog', ['syslogmodule.c']) )

        # George Neville-Neil's timing module:
        # Deprecated in PEP 4 http://www.python.org/peps/pep-0004.html
        # http://mail.python.org/pipermail/python-dev/2006-January/060023.html
        #exts.append( Extension('timing', ['timingmodule.c']) )

        #
        # Here ends the simple stuff.  From here on, modules need certain
        # libraries, are platform-specific, or present other surprises.
        #

        # Multimedia modules
        # These don't work for 64-bit platforms!!!
        # These represent audio samples or images as strings:

        # Operations on audio samples
        # According to #993173, this one should actually work fine on
        # 64-bit platforms.
        # Pyston change: disable the audioop modules in this setup.py
        # exts.append( Extension('audioop', ['audioop.c']) )

        # Disabled on 64-bit platforms
        if sys.maxint != 9223372036854775807L:
            # Operations on images
            exts.append( Extension('imageop', ['imageop.c']) )
        else:
            missing.extend(['imageop'])

        # readline
        do_readline = self.compiler.find_library_file(lib_dirs, 'readline')
        readline_termcap_library = ""
        curses_library = ""
        # Determine if readline is already linked against curses or tinfo.
        if do_readline and find_executable('ldd'):
            fp = os.popen("ldd %s" % do_readline)
            ldd_output = fp.readlines()
            ret = fp.close()
            if ret is None or ret >> 8 == 0:
                for ln in ldd_output:
                    if 'curses' in ln:
                        readline_termcap_library = re.sub(
                            r'.*lib(n?cursesw?)\.so.*', r'\1', ln
                        ).rstrip()
                        break
                    if 'tinfo' in ln: # termcap interface split out from ncurses
                        readline_termcap_library = 'tinfo'
                        break
        # Issue 7384: If readline is already linked against curses,
        # use the same library for the readline and curses modules.
        if 'curses' in readline_termcap_library:
            curses_library = readline_termcap_library
        elif self.compiler.find_library_file(lib_dirs, 'ncursesw'):
            curses_library = 'ncursesw'
        elif self.compiler.find_library_file(lib_dirs, 'ncurses'):
            curses_library = 'ncurses'
        elif self.compiler.find_library_file(lib_dirs, 'curses'):
            curses_library = 'curses'

        if host_platform == 'darwin':
            os_release = int(os.uname()[2].split('.')[0])
            dep_target = sysconfig.get_config_var('MACOSX_DEPLOYMENT_TARGET')
            if dep_target and dep_target.split('.') < ['10', '5']:
                os_release = 8
            if os_release < 9:
                # MacOSX 10.4 has a broken readline. Don't try to build
                # the readline module unless the user has installed a fixed
                # readline package
                if find_file('readline/rlconf.h', inc_dirs, []) is None:
                    do_readline = False
        if do_readline:
            if host_platform == 'darwin' and os_release < 9:
                # In every directory on the search path search for a dynamic
                # library and then a static library, instead of first looking
                # for dynamic libraries on the entiry path.
                # This way a staticly linked custom readline gets picked up
                # before the (possibly broken) dynamic library in /usr/lib.
                readline_extra_link_args = ('-Wl,-search_paths_first',)
            else:
                readline_extra_link_args = ()

            readline_libs = ['readline']
            if readline_termcap_library:
                pass # Issue 7384: Already linked against curses or tinfo.
            elif curses_library:
                readline_libs.append(curses_library)
            elif self.compiler.find_library_file(lib_dirs +
                                                     ['/usr/lib/termcap'],
                                                     'termcap'):
                readline_libs.append('termcap')
            exts.append( Extension('readline', sources = map(relpath, [
                "Modules/readline.c",
            ])))
        else:
            missing.append('readline')

        # Pyston change: disable the crypt module in this setup.py
        # crypt module.
        # if self.compiler.find_library_file(lib_dirs, 'crypt'):
        #     libs = ['crypt']
        # else:
        #     libs = []
        # exts.append( Extension('crypt', ['cryptmodule.c'], libraries=libs) )

        # Pyston change: handled by from_cpython/CMakefile.txt
        # CSV files
        # exts.append( Extension('_csv', ['_csv.c']) )

        # Pyston change: disable the _socket module in this setup.py
        # socket(2)
        # exts.append( Extension('_socket', ['socketmodule.c', 'timemodule.c'],
        #                        depends=['socketmodule.h'],
        #                        libraries=math_libs) )
        # Detect SSL support for the socket module (via _ssl)
        search_for_ssl_incs_in = [
                              '/usr/local/ssl/include',
                              '/usr/contrib/ssl/include/'
                             ]
        ssl_incs = find_file('openssl/ssl.h', inc_dirs,
                             search_for_ssl_incs_in
                             )
        if ssl_incs is not None:
            krb5_h = find_file('krb5.h', inc_dirs,
                               ['/usr/kerberos/include'])
            if krb5_h:
                ssl_incs += krb5_h
        ssl_libs = find_library_file(self.compiler, 'ssl',lib_dirs,
                                     ['/usr/local/ssl/lib',
                                      '/usr/contrib/ssl/lib/'
                                     ] )

        # Pyston change: the _ssl module is handled by
        # from_cpython/CMakefiles.txy
        # if (ssl_incs is not None and
        #     ssl_libs is not None):
        #     exts.append( Extension('_ssl', ['_ssl.c'],
        #                            include_dirs = ssl_incs,
        #                            library_dirs = ssl_libs,
        #                            libraries = ['ssl', 'crypto'],
        #                            depends = ['socketmodule.h']), )
        # else:
        #     missing.append('_ssl')

        # find out which version of OpenSSL we have
        openssl_ver = 0
        openssl_ver_re = re.compile(
            '^\s*#\s*define\s+OPENSSL_VERSION_NUMBER\s+(0x[0-9a-fA-F]+)' )

        # look for the openssl version header on the compiler search path.
        opensslv_h = find_file('openssl/opensslv.h', [],
                inc_dirs + search_for_ssl_incs_in)
        if opensslv_h:
            name = os.path.join(opensslv_h[0], 'openssl/opensslv.h')
            if host_platform == 'darwin' and is_macosx_sdk_path(name):
                name = os.path.join(macosx_sdk_root(), name[1:])
            try:
                incfile = open(name, 'r')
                for line in incfile:
                    m = openssl_ver_re.match(line)
                    if m:
                        openssl_ver = eval(m.group(1))
            except IOError, msg:
                print "IOError while reading opensshv.h:", msg
                pass

        min_openssl_ver = 0x00907000
        have_any_openssl = ssl_incs is not None and ssl_libs is not None
        have_usable_openssl = (have_any_openssl and
                               openssl_ver >= min_openssl_ver)

        # Pyston change: didn't support hashlib yet.
        # if have_any_openssl:
        #     if have_usable_openssl:
        #         # The _hashlib module wraps optimized implementations
        #         # of hash functions from the OpenSSL library.
        #         exts.append( Extension('_hashlib', ['_hashopenssl.c'],
        #                                include_dirs = ssl_incs,
        #                                library_dirs = ssl_libs,
        #                                libraries = ['ssl', 'crypto']) )
        #     else:
        #         print ("warning: openssl 0x%08x is too old for _hashlib" %
        #                openssl_ver)
        #         missing.append('_hashlib')
        # Pyston change: the shaXXX module handled by from_cpython/CMakefile
        # if COMPILED_WITH_PYDEBUG or not have_usable_openssl:
        #     # The _sha module implements the SHA1 hash algorithm.
        #     exts.append( Extension('_sha', ['shamodule.c']) )

        # Pyston change: the md5 module handled by from_cpython/CMakefile
        #     # The _md5 module implements the RSA Data Security, Inc. MD5
        #     # Message-Digest Algorithm, described in RFC 1321.  The
        #     # necessary files md5.c and md5.h are included here.
        #     exts.append( Extension('_md5',
        #                     sources = ['md5module.c', 'md5.c'],
        #                     depends = ['md5.h']) )
        #
        # Pyston change: the shaXXX module handled by from_cpython/CMakefile
        # min_sha2_openssl_ver = 0x00908000
        # if COMPILED_WITH_PYDEBUG or openssl_ver < min_sha2_openssl_ver:
        #     # OpenSSL doesn't do these until 0.9.8 so we'll bring our own hash
        #     exts.append( Extension('_sha256', ['sha256module.c']) )
        #     exts.append( Extension('_sha512', ['sha512module.c']) )

        # Modules that provide persistent dictionary-like semantics.  You will
        # probably want to arrange for at least one of them to be available on
        # your machine, though none are defined by default because of library
        # dependencies.  The Python module anydbm.py provides an
        # implementation independent wrapper for these; dumbdbm.py provides
        # similar functionality (but slower of course) implemented in Python.

        # Sleepycat^WOracle Berkeley DB interface.
        #  http://www.oracle.com/database/berkeley-db/db/index.html
        #
        # This requires the Sleepycat^WOracle DB code. The supported versions
        # are set below.  Visit the URL above to download
        # a release.  Most open source OSes come with one or more
        # versions of BerkeleyDB already installed.

        max_db_ver = (5, 3)
        min_db_ver = (4, 3)
        db_setup_debug = False   # verbose debug prints from this script?

        def allow_db_ver(db_ver):
            """Returns a boolean if the given BerkeleyDB version is acceptable.

            Args:
              db_ver: A tuple of the version to verify.
            """
            if not (min_db_ver <= db_ver <= max_db_ver):
                return False
            # Use this function to filter out known bad configurations.
            if (4, 6) == db_ver[:2]:
                # BerkeleyDB 4.6.x is not stable on many architectures.
                arch = platform_machine()
                if arch not in ('i386', 'i486', 'i586', 'i686',
                                'x86_64', 'ia64'):
                    return False
            return True

        def gen_db_minor_ver_nums(major):
            if major == 5:
                for x in range(max_db_ver[1]+1):
                    if allow_db_ver((5, x)):
                        yield x
            elif major == 4:
                for x in range(9):
                    if allow_db_ver((4, x)):
                        yield x
            elif major == 3:
                for x in (3,):
                    if allow_db_ver((3, x)):
                        yield x
            else:
                raise ValueError("unknown major BerkeleyDB version", major)

        # construct a list of paths to look for the header file in on
        # top of the normal inc_dirs.
        db_inc_paths = [
            '/usr/include/db4',
            '/usr/local/include/db4',
            '/opt/sfw/include/db4',
            '/usr/include/db3',
            '/usr/local/include/db3',
            '/opt/sfw/include/db3',
            # Fink defaults (http://fink.sourceforge.net/)
            '/sw/include/db4',
            '/sw/include/db3',
        ]
        # 4.x minor number specific paths
        for x in gen_db_minor_ver_nums(4):
            db_inc_paths.append('/usr/include/db4%d' % x)
            db_inc_paths.append('/usr/include/db4.%d' % x)
            db_inc_paths.append('/usr/local/BerkeleyDB.4.%d/include' % x)
            db_inc_paths.append('/usr/local/include/db4%d' % x)
            db_inc_paths.append('/pkg/db-4.%d/include' % x)
            db_inc_paths.append('/opt/db-4.%d/include' % x)
            # MacPorts default (http://www.macports.org/)
            db_inc_paths.append('/opt/local/include/db4%d' % x)
        # 3.x minor number specific paths
        for x in gen_db_minor_ver_nums(3):
            db_inc_paths.append('/usr/include/db3%d' % x)
            db_inc_paths.append('/usr/local/BerkeleyDB.3.%d/include' % x)
            db_inc_paths.append('/usr/local/include/db3%d' % x)
            db_inc_paths.append('/pkg/db-3.%d/include' % x)
            db_inc_paths.append('/opt/db-3.%d/include' % x)

        if cross_compiling:
            db_inc_paths = []

        # Add some common subdirectories for Sleepycat DB to the list,
        # based on the standard include directories. This way DB3/4 gets
        # picked up when it is installed in a non-standard prefix and
        # the user has added that prefix into inc_dirs.
        std_variants = []
        for dn in inc_dirs:
            std_variants.append(os.path.join(dn, 'db3'))
            std_variants.append(os.path.join(dn, 'db4'))
            for x in gen_db_minor_ver_nums(4):
                std_variants.append(os.path.join(dn, "db4%d"%x))
                std_variants.append(os.path.join(dn, "db4.%d"%x))
            for x in gen_db_minor_ver_nums(3):
                std_variants.append(os.path.join(dn, "db3%d"%x))
                std_variants.append(os.path.join(dn, "db3.%d"%x))

        db_inc_paths = std_variants + db_inc_paths
        db_inc_paths = [p for p in db_inc_paths if os.path.exists(p)]

        db_ver_inc_map = {}

        if host_platform == 'darwin':
            sysroot = macosx_sdk_root()

        class db_found(Exception): pass
        try:
            # See whether there is a Sleepycat header in the standard
            # search path.
            for d in inc_dirs + db_inc_paths:
                f = os.path.join(d, "db.h")

                if host_platform == 'darwin' and is_macosx_sdk_path(d):
                    f = os.path.join(sysroot, d[1:], "db.h")

                if db_setup_debug: print "db: looking for db.h in", f
                if os.path.exists(f):
                    f = open(f).read()
                    m = re.search(r"#define\WDB_VERSION_MAJOR\W(\d+)", f)
                    if m:
                        db_major = int(m.group(1))
                        m = re.search(r"#define\WDB_VERSION_MINOR\W(\d+)", f)
                        db_minor = int(m.group(1))
                        db_ver = (db_major, db_minor)

                        # Avoid 4.6 prior to 4.6.21 due to a BerkeleyDB bug
                        if db_ver == (4, 6):
                            m = re.search(r"#define\WDB_VERSION_PATCH\W(\d+)", f)
                            db_patch = int(m.group(1))
                            if db_patch < 21:
                                print "db.h:", db_ver, "patch", db_patch,
                                print "being ignored (4.6.x must be >= 4.6.21)"
                                continue

                        if ( (db_ver not in db_ver_inc_map) and
                            allow_db_ver(db_ver) ):
                            # save the include directory with the db.h version
                            # (first occurrence only)
                            db_ver_inc_map[db_ver] = d
                            if db_setup_debug:
                                print "db.h: found", db_ver, "in", d
                        else:
                            # we already found a header for this library version
                            if db_setup_debug: print "db.h: ignoring", d
                    else:
                        # ignore this header, it didn't contain a version number
                        if db_setup_debug:
                            print "db.h: no version number version in", d

            db_found_vers = db_ver_inc_map.keys()
            db_found_vers.sort()

            while db_found_vers:
                db_ver = db_found_vers.pop()
                db_incdir = db_ver_inc_map[db_ver]

                # check lib directories parallel to the location of the header
                db_dirs_to_check = [
                    db_incdir.replace("include", 'lib64'),
                    db_incdir.replace("include", 'lib'),
                ]

                if host_platform != 'darwin':
                    db_dirs_to_check = filter(os.path.isdir, db_dirs_to_check)

                else:
                    # Same as other branch, but takes OSX SDK into account
                    tmp = []
                    for dn in db_dirs_to_check:
                        if is_macosx_sdk_path(dn):
                            if os.path.isdir(os.path.join(sysroot, dn[1:])):
                                tmp.append(dn)
                        else:
                            if os.path.isdir(dn):
                                tmp.append(dn)
                    db_dirs_to_check = tmp

                # Look for a version specific db-X.Y before an ambiguous dbX
                # XXX should we -ever- look for a dbX name?  Do any
                # systems really not name their library by version and
                # symlink to more general names?
                for dblib in (('db-%d.%d' % db_ver),
                              ('db%d%d' % db_ver),
                              ('db%d' % db_ver[0])):
                    dblib_file = self.compiler.find_library_file(
                                    db_dirs_to_check + lib_dirs, dblib )
                    if dblib_file:
                        dblib_dir = [ os.path.abspath(os.path.dirname(dblib_file)) ]
                        raise db_found
                    else:
                        if db_setup_debug: print "db lib: ", dblib, "not found"

        except db_found:
            if db_setup_debug:
                print "bsddb using BerkeleyDB lib:", db_ver, dblib
                print "bsddb lib dir:", dblib_dir, " inc dir:", db_incdir
            db_incs = [db_incdir]
            dblibs = [dblib]
            # We add the runtime_library_dirs argument because the
            # BerkeleyDB lib we're linking against often isn't in the
            # system dynamic library search path.  This is usually
            # correct and most trouble free, but may cause problems in
            # some unusual system configurations (e.g. the directory
            # is on an NFS server that goes away).
        # Pyston change: we don't suport bsddb module yet.
        #     exts.append(Extension('_bsddb', ['_bsddb.c'],
        #                           depends = ['bsddb.h'],
        #                           library_dirs=dblib_dir,
        #                           runtime_library_dirs=dblib_dir,
        #                           include_dirs=db_incs,
        #                           libraries=dblibs))
        else:
            if db_setup_debug: print "db: no appropriate library found"
            db_incs = None
            dblibs = []
            dblib_dir = None
            missing.append('_bsddb')

        # The sqlite interface
        sqlite_setup_debug = False   # verbose debug prints from this script?

        # We hunt for #define SQLITE_VERSION "n.n.n"
        # We need to find >= sqlite version 3.0.8
        sqlite_incdir = sqlite_libdir = None
        sqlite_inc_paths = [ '/usr/include',
                             '/usr/include/sqlite',
                             '/usr/include/sqlite3',
                             '/usr/local/include',
                             '/usr/local/include/sqlite',
                             '/usr/local/include/sqlite3',
                           ]
        if cross_compiling:
            sqlite_inc_paths = []
        MIN_SQLITE_VERSION_NUMBER = (3, 0, 8)
        MIN_SQLITE_VERSION = ".".join([str(x)
                                    for x in MIN_SQLITE_VERSION_NUMBER])

        # Scan the default include directories before the SQLite specific
        # ones. This allows one to override the copy of sqlite on OSX,
        # where /usr/include contains an old version of sqlite.
        if host_platform == 'darwin':
            sysroot = macosx_sdk_root()

        for d_ in inc_dirs + sqlite_inc_paths:
            d = d_
            if host_platform == 'darwin' and is_macosx_sdk_path(d):
                d = os.path.join(sysroot, d[1:])

            f = os.path.join(d, "sqlite3.h")
            if os.path.exists(f):
                if sqlite_setup_debug: print "sqlite: found %s"%f
                incf = open(f).read()
                m = re.search(
                    r'\s*.*#\s*.*define\s.*SQLITE_VERSION\W*"([\d\.]*)"', incf)
                if m:
                    sqlite_version = m.group(1)
                    sqlite_version_tuple = tuple([int(x)
                                        for x in sqlite_version.split(".")])
                    if sqlite_version_tuple >= MIN_SQLITE_VERSION_NUMBER:
                        # we win!
                        if sqlite_setup_debug:
                            print "%s/sqlite3.h: version %s"%(d, sqlite_version)
                        sqlite_incdir = d
                        break
                    else:
                        if sqlite_setup_debug:
                            print "%s: version %d is too old, need >= %s"%(d,
                                        sqlite_version, MIN_SQLITE_VERSION)
                elif sqlite_setup_debug:
                    print "sqlite: %s had no SQLITE_VERSION"%(f,)

        if sqlite_incdir:
            sqlite_dirs_to_check = [
                os.path.join(sqlite_incdir, '..', 'lib64'),
                os.path.join(sqlite_incdir, '..', 'lib'),
                os.path.join(sqlite_incdir, '..', '..', 'lib64'),
                os.path.join(sqlite_incdir, '..', '..', 'lib'),
            ]
            sqlite_libfile = self.compiler.find_library_file(
                                sqlite_dirs_to_check + lib_dirs, 'sqlite3')
            if sqlite_libfile:
                sqlite_libdir = [os.path.abspath(os.path.dirname(sqlite_libfile))]

        if sqlite_incdir and sqlite_libdir:
            sqlite_srcs = ['_sqlite/cache.c',
                '_sqlite/connection.c',
                '_sqlite/cursor.c',
                '_sqlite/microprotocols.c',
                '_sqlite/module.c',
                '_sqlite/prepare_protocol.c',
                '_sqlite/row.c',
                '_sqlite/statement.c',
                '_sqlite/util.c', ]

            sqlite_defines = []
            if host_platform != "win32":
                sqlite_defines.append(('MODULE_NAME', '"sqlite3"'))
            else:
                sqlite_defines.append(('MODULE_NAME', '\\"sqlite3\\"'))

            # Comment this out if you want the sqlite3 module to be able to load extensions.
            sqlite_defines.append(("SQLITE_OMIT_LOAD_EXTENSION", "1"))

            if host_platform == 'darwin':
                # In every directory on the search path search for a dynamic
                # library and then a static library, instead of first looking
                # for dynamic libraries on the entire path.
                # This way a statically linked custom sqlite gets picked up
                # before the dynamic library in /usr/lib.
                sqlite_extra_link_args = ('-Wl,-search_paths_first',)
            else:
                sqlite_extra_link_args = ()

        # Pyston change: we don't suport sqlite yet.
        #     exts.append(Extension('_sqlite3', sqlite_srcs,
        #                           define_macros=sqlite_defines,
        #                           include_dirs=["Modules/_sqlite",
        #                                         sqlite_incdir],
        #                           library_dirs=sqlite_libdir,
        #                           extra_link_args=sqlite_extra_link_args,
        #                           libraries=["sqlite3",]))
        # else:
        #     missing.append('_sqlite3')

        # Look for Berkeley db 1.85.   Note that it is built as a different
        # module name so it can be included even when later versions are
        # available.  A very restrictive search is performed to avoid
        # accidentally building this module with a later version of the
        # underlying db library.  May BSD-ish Unixes incorporate db 1.85
        # symbols into libc and place the include file in /usr/include.
        #
        # If the better bsddb library can be built (db_incs is defined)
        # we do not build this one.  Otherwise this build will pick up
        # the more recent berkeleydb's db.h file first in the include path
        # when attempting to compile and it will fail.
        f = "/usr/include/db.h"

        if host_platform == 'darwin':
            if is_macosx_sdk_path(f):
                sysroot = macosx_sdk_root()
                f = os.path.join(sysroot, f[1:])

        if os.path.exists(f) and not db_incs:
            data = open(f).read()
            m = re.search(r"#s*define\s+HASHVERSION\s+2\s*", data)
            if m is not None:
                # bingo - old version used hash file format version 2
                ### XXX this should be fixed to not be platform-dependent
                ### but I don't have direct access to an osf1 platform and
                ### seemed to be muffing the search somehow
                libraries = host_platform == "osf1" and ['db'] or None
                if libraries is not None:
                    exts.append(Extension('bsddb185', ['bsddbmodule.c'],
                                          libraries=libraries))
                else:
                    exts.append(Extension('bsddb185', ['bsddbmodule.c']))
            else:
                missing.append('bsddb185')
        else:
            missing.append('bsddb185')

        dbm_order = ['gdbm']
        # The standard Unix dbm module:
        if host_platform not in ['cygwin']:
            # Pyston change: hard code the dbm_args, Pyston don't support
            # CONFIG_ARGS yet.
            dbm_args = None
            # config_args = [arg.strip("'")
            #                for arg in sysconfig.get_config_var("CONFIG_ARGS").split()]
            # dbm_args = [arg for arg in config_args
            #             if arg.startswith('--with-dbmliborder=')]
            if dbm_args:
                dbm_order = [arg.split('=')[-1] for arg in dbm_args][-1].split(":")
            else:
                dbm_order = "ndbm:gdbm:bdb".split(":")
            dbmext = None
            for cand in dbm_order:
                if cand == "ndbm":
                    if find_file("ndbm.h", inc_dirs, []) is not None:
                        # Some systems have -lndbm, others have -lgdbm_compat,
                        # others don't have either
                        if self.compiler.find_library_file(lib_dirs,
                                                               'ndbm'):
                            ndbm_libs = ['ndbm']
                        elif self.compiler.find_library_file(lib_dirs,
                                                             'gdbm_compat'):
                            ndbm_libs = ['gdbm_compat']
                        else:
                            ndbm_libs = []
                        print "building dbm using ndbm"
                        dbmext = Extension('dbm', ['dbmmodule.c'],
                                           define_macros=[
                                               ('HAVE_NDBM_H',None),
                                               ],
                                           libraries=ndbm_libs)
                        break

                elif cand == "gdbm":
                    if self.compiler.find_library_file(lib_dirs, 'gdbm'):
                        gdbm_libs = ['gdbm']
                        if self.compiler.find_library_file(lib_dirs,
                                                               'gdbm_compat'):
                            gdbm_libs.append('gdbm_compat')
                        if find_file("gdbm/ndbm.h", inc_dirs, []) is not None:
                            print "building dbm using gdbm"
                            dbmext = Extension(
                                'dbm', ['dbmmodule.c'],
                                define_macros=[
                                    ('HAVE_GDBM_NDBM_H', None),
                                    ],
                                libraries = gdbm_libs)
                            break
                        if find_file("gdbm-ndbm.h", inc_dirs, []) is not None:
                            print "building dbm using gdbm"
                            dbmext = Extension(
                                'dbm', ['dbmmodule.c'],
                                define_macros=[
                                    ('HAVE_GDBM_DASH_NDBM_H', None),
                                    ],
                                libraries = gdbm_libs)
                            break
                elif cand == "bdb":
                    if db_incs is not None:
                        print "building dbm using bdb"
                        dbmext = Extension('dbm', ['dbmmodule.c'],
                                           library_dirs=dblib_dir,
                                           runtime_library_dirs=dblib_dir,
                                           include_dirs=db_incs,
                                           define_macros=[
                                               ('HAVE_BERKDB_H', None),
                                               ('DB_DBM_HSEARCH', None),
                                               ],
                                           libraries=dblibs)
                        break

        # Pyston change: we don't support those dbm modules yet
        #     if dbmext is not None:
        #         exts.append(dbmext)
        #     else:
        #         missing.append('dbm')
        #
        # # Anthony Baxter's gdbm module.  GNU dbm(3) will require -lgdbm:
        # if ('gdbm' in dbm_order and
        #     self.compiler.find_library_file(lib_dirs, 'gdbm')):
        #     exts.append( Extension('gdbm', ['gdbmmodule.c'],
        #                            libraries = ['gdbm'] ) )
        # else:
        #     missing.append('gdbm')

        # Unix-only modules
        if host_platform not in ['win32']:
            # Steen Lumholt's termios module
            exts.append( Extension('termios', sources = map(relpath, [
                "Modules/termios.c",
            ])) )

            # Pyston change: the resource module handled by from_cpython/CMakefile
            # # Jeremy Hylton's rlimit interface
            # if host_platform not in ['atheos']:
            #     exts.append( Extension('resource', ['resource.c']) )
            # else:
            #     missing.append('resource')
            #
            # Pyston change: we don't support nis module yet
            # # Sun yellow pages. Some systems have the functions in libc.
            # if (host_platform not in ['cygwin', 'atheos', 'qnx6'] and
            #     find_file('rpcsvc/yp_prot.h', inc_dirs, []) is not None):
            #     if (self.compiler.find_library_file(lib_dirs, 'nsl')):
            #         libs = ['nsl']
            #     else:
            #         libs = []
            #     exts.append( Extension('nis', ['nismodule.c'],
            #                            libraries = libs) )
            # else:
            #     missing.append('nis')
        else:
            missing.extend(['nis', 'resource', 'termios'])

        # Curses support, requiring the System V version of curses, often
        # provided by the ncurses library.
        panel_library = 'panel'
        curses_incs = None
        if curses_library.startswith('ncurses'):
            if curses_library == 'ncursesw':
                # Bug 1464056: If _curses.so links with ncursesw,
                # _curses_panel.so must link with panelw.
                panel_library = 'panelw'
            curses_libs = [curses_library]
            curses_incs = find_file('curses.h', inc_dirs,
                                    [os.path.join(d, 'ncursesw') for d in inc_dirs])
            exts.append( Extension('_curses',
                                   sources = map(relpath,
                                                 [ "Modules/_cursesmodule.c", ]),
                                   include_dirs = curses_incs,
                                   libraries = curses_libs) )
        elif curses_library == 'curses' and host_platform != 'darwin':
                # OSX has an old Berkeley curses, not good enough for
                # the _curses module.
            if (self.compiler.find_library_file(lib_dirs, 'terminfo')):
                curses_libs = ['curses', 'terminfo']
            elif (self.compiler.find_library_file(lib_dirs, 'termcap')):
                curses_libs = ['curses', 'termcap']
            else:
                curses_libs = ['curses']

            exts.append( Extension('_curses', ['_cursesmodule.c'],
                                   libraries = curses_libs) )
        else:
            missing.append('_curses')
        # Pyston change: disable these modules in this setup.py
        # # If the curses module is enabled, check for the panel module
        # if (module_enabled(exts, '_curses') and
        #     self.compiler.find_library_file(lib_dirs, panel_library)):
        #     exts.append( Extension('_curses_panel', ['_curses_panel.c'],
        #                            include_dirs = curses_incs,
        #                            libraries = [panel_library] + curses_libs) )
        # else:
        #     missing.append('_curses_panel')
        #
        # Pyston change: the zlib module is handled by from_cpython/CMakefile
        # And the code of build zlib in below is untested.
        # Andrew Kuchling's zlib module.  Note that some versions of zlib
        # 1.1.3 have security problems.  See CERT Advisory CA-2002-07:
        # http://www.cert.org/advisories/CA-2002-07.html
        #
        # zlib 1.1.4 is fixed, but at least one vendor (RedHat) has decided to
        # patch its zlib 1.1.3 package instead of upgrading to 1.1.4.  For
        # now, we still accept 1.1.3, because we think it's difficult to
        # exploit this in Python, and we'd rather make it RedHat's problem
        # than our problem <wink>.
        #
        # You can upgrade zlib to version 1.1.4 yourself by going to
        # http://www.gzip.org/zlib/
        zlib_inc = find_file('zlib.h', [], inc_dirs)
        have_zlib = False
        if zlib_inc is not None:
            zlib_h = zlib_inc[0] + '/zlib.h'
            version = '"0.0.0"'
            version_req = '"1.1.3"'
            if host_platform == 'darwin' and is_macosx_sdk_path(zlib_h):
                zlib_h = os.path.join(macosx_sdk_root(), zlib_h[1:])
            fp = open(zlib_h)
            while 1:
                line = fp.readline()
                if not line:
                    break
                if line.startswith('#define ZLIB_VERSION'):
                    version = line.split()[2]
                    break
            if version >= version_req:
                if (self.compiler.find_library_file(lib_dirs, 'z')):
                    if host_platform == "darwin":
                        zlib_extra_link_args = ('-Wl,-search_paths_first',)
                    else:
                        zlib_extra_link_args = ()
                    exts.append( Extension('zlib', ['zlibmodule.c'],
                                           libraries = ['z'],
                                           extra_link_args = zlib_extra_link_args))
                    have_zlib = True
                else:
                    missing.append('zlib')
            else:
                missing.append('zlib')
        else:
            missing.append('zlib')

        # Pyston change: handled by from_cpython/CMakefile
        # # Helper module for various ascii-encoders.  Uses zlib for an optimized
        # # crc32 if we have it.  Otherwise binascii uses its own.
        # if have_zlib:
        #     extra_compile_args = ['-DUSE_ZLIB_CRC32']
        #     libraries = ['z']
        #     extra_link_args = zlib_extra_link_args
        # else:
        #     extra_compile_args = []
        #     libraries = []
        #     extra_link_args = []
        # exts.append( Extension('binascii', ['binascii.c'],
        #                        extra_compile_args = extra_compile_args,
        #                        libraries = libraries,
        #                        extra_link_args = extra_link_args) )
        #
        # Gustavo Niemeyer's bz2 module.
        if (self.compiler.find_library_file(lib_dirs, 'bz2')):
            if host_platform == "darwin":
                bz2_extra_link_args = ('-Wl,-search_paths_first',)
            else:
                bz2_extra_link_args = ()
            exts.append( Extension('bz2', sources = map(relpath,
                                                        [ "Modules/bz2module.c", ]),
                                   libraries = ['bz2'],
                                   extra_link_args = bz2_extra_link_args) )
        else:
            missing.append('bz2')

        # Interface to the Expat XML parser
        #
        # Expat was written by James Clark and is now maintained by a group of
        # developers on SourceForge; see www.libexpat.org for more information.
        # The pyexpat module was written by Paul Prescod after a prototype by
        # Jack Jansen.  The Expat source is included in Modules/expat/.  Usage
        # of a system shared libexpat.so is possible with --with-system-expat
        # configure option.
        #
        # More information on Expat can be found at www.libexpat.org.
        #

        # Pyston change: Pyston don't support CONFIG_ARGS yet
        # if '--with-system-expat' in sysconfig.get_config_var("CONFIG_ARGS"):
        #     expat_inc = []
        #     define_macros = []
        #     expat_lib = ['expat']
        #     expat_sources = []
        #     expat_depends = []
        # else:
        #     expat_inc = [os.path.join(os.getcwd(), srcdir, 'Modules', 'expat')]
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
        exts.append(Extension('pyexpat',
                              define_macros = define_macros,
                              include_dirs = [relpath('Modules/expat')],
                              sources = expat_sources,
                              depends = expat_depends,
                              ))
        # Pyston change: we don't need this complicated configuration for now.
        #     expat_inc = [os.path.join(os.getcwd(), srcdir, 'Modules', 'expat')]

        # Fredrik Lundh's cElementTree module.  Note that this also
        # uses expat (via the CAPI hook in pyexpat).
        #
        # if os.path.isfile(os.path.join(srcdir, 'Modules', '_elementtree.c')):
        #     define_macros.append(('USE_PYEXPAT_CAPI', None))
        # elementtree depends on expat

        define_macros = define_macros + [('USE_PYEXPAT_CAPI', None),]

        exts.append(Extension('_elementtree',
                              define_macros = define_macros,
                              include_dirs = [relpath('Modules/expat')],
                              # libraries = pyexpat.libraries,
                              sources = [relpath('Modules/_elementtree.c')],
                              depends = expat_depends,
                              ))
        # else:
        #     missing.append('_elementtree')

        # Pyston change: disable those modules in setup.py for now
        # # Hye-Shik Chang's CJKCodecs modules.
        # if have_unicode:
        #     exts.append(Extension('_multibytecodec',
        #                           ['cjkcodecs/multibytecodec.c']))
        #     for loc in ('kr', 'jp', 'cn', 'tw', 'hk', 'iso2022'):
        #         exts.append(Extension('_codecs_%s' % loc,
        #                               ['cjkcodecs/_codecs_%s.c' % loc]))
        # else:
        #     missing.append('_multibytecodec')
        #     for loc in ('kr', 'jp', 'cn', 'tw', 'hk', 'iso2022'):
        #         missing.append('_codecs_%s' % loc)
        # #
        # # Dynamic loading module
        # if sys.maxint == 0x7fffffff:
        #     # This requires sizeof(int) == sizeof(long) == sizeof(char*)
        #     dl_inc = find_file('dlfcn.h', [], inc_dirs)
        #     if (dl_inc is not None) and (host_platform not in ['atheos']):
        #         exts.append( Extension('dl', ['dlmodule.c']) )
        #     else:
        #         missing.append('dl')
        # else:
        #     missing.append('dl')
        #
        # Thomas Heller's _ctypes module
        self.detect_ctypes(inc_dirs, lib_dirs)

        # Richard Oudkerk's multiprocessing module
        if host_platform == 'win32':             # Windows
            macros = dict()
            libraries = ['ws2_32']

        elif host_platform == 'darwin':          # Mac OSX
            macros = dict()
            libraries = []

        elif host_platform == 'cygwin':          # Cygwin
            macros = dict()
            libraries = []

        elif host_platform in ('freebsd4', 'freebsd5', 'freebsd6', 'freebsd7', 'freebsd8'):
            # FreeBSD's P1003.1b semaphore support is very experimental
            # and has many known problems. (as of June 2008)
            macros = dict()
            libraries = []

        elif host_platform.startswith('openbsd'):
            macros = dict()
            libraries = []

        elif host_platform.startswith('netbsd'):
            macros = dict()
            libraries = []

        else:                                   # Linux and other unices
            macros = dict()
            libraries = ['rt']

        if host_platform == 'win32':
            multiprocessing_srcs = [ '_multiprocessing/multiprocessing.c',
                                     '_multiprocessing/semaphore.c',
                                     '_multiprocessing/pipe_connection.c',
                                     '_multiprocessing/socket_connection.c',
                                     '_multiprocessing/win32_functions.c'
                                   ]

        else:
            multiprocessing_srcs = map(relpath, [
                "Modules/_multiprocessing/multiprocessing.c",
                "Modules/_multiprocessing/socket_connection.c",
                "Modules/_multiprocessing/semaphore.c",
            ])
        # if sysconfig.get_config_var('WITH_THREAD'):
        exts.append( Extension('_multiprocessing', multiprocessing_srcs,
                                include_dirs=["Modules/_multiprocessing"]))
        # else:
        #     missing.append('_multiprocessing')

        # End multiprocessing

        # Pyston change: disable these modules in this setup.py
        # # Platform-specific libraries
        # if host_platform == 'linux2':
        #     # Linux-specific modules
        #     exts.append( Extension('linuxaudiodev', ['linuxaudiodev.c']) )
        # else:
        #     missing.append('linuxaudiodev')
        #
        # if (host_platform in ('linux2', 'freebsd4', 'freebsd5', 'freebsd6',
        #                 'freebsd7', 'freebsd8')
        #     or host_platform.startswith("gnukfreebsd")):
        #     exts.append( Extension('ossaudiodev', ['ossaudiodev.c']) )
        # else:
        #     missing.append('ossaudiodev')
        #
        # if host_platform == 'sunos5':
        #     # SunOS specific modules
        #     exts.append( Extension('sunaudiodev', ['sunaudiodev.c']) )
        # else:
        #     missing.append('sunaudiodev')

        if host_platform == 'darwin':
            # _scproxy
            exts.append(Extension("_scproxy", [os.path.join(srcdir, "Mac/Modules/_scproxy.c")],
                extra_link_args= [
                    '-framework', 'SystemConfiguration',
                    '-framework', 'CoreFoundation'
                ]))


        # Pyston change: this code is untested in Mac
        if host_platform == 'darwin' and ("--disable-toolbox-glue" not in
                sysconfig.get_config_var("CONFIG_ARGS")):

            if int(os.uname()[2].split('.')[0]) >= 8:
                # We're on Mac OS X 10.4 or later, the compiler should
                # support '-Wno-deprecated-declarations'. This will
                # surpress deprecation warnings for the Carbon extensions,
                # these extensions wrap the Carbon APIs and even those
                # parts that are deprecated.
                carbon_extra_compile_args = ['-Wno-deprecated-declarations']
            else:
                carbon_extra_compile_args = []

            # Mac OS X specific modules.
            def macSrcExists(name1, name2=''):
                if not name1:
                    return None
                names = (name1,)
                if name2:
                    names = (name1, name2)
                path = os.path.join(srcdir, 'Mac', 'Modules', *names)
                return os.path.exists(path)

            def addMacExtension(name, kwds, extra_srcs=[]):
                dirname = ''
                if name[0] == '_':
                    dirname = name[1:].lower()
                cname = name + '.c'
                cmodulename = name + 'module.c'
                # Check for NNN.c, NNNmodule.c, _nnn/NNN.c, _nnn/NNNmodule.c
                if macSrcExists(cname):
                    srcs = [cname]
                elif macSrcExists(cmodulename):
                    srcs = [cmodulename]
                elif macSrcExists(dirname, cname):
                    # XXX(nnorwitz): If all the names ended with module, we
                    # wouldn't need this condition.  ibcarbon is the only one.
                    srcs = [os.path.join(dirname, cname)]
                elif macSrcExists(dirname, cmodulename):
                    srcs = [os.path.join(dirname, cmodulename)]
                else:
                    raise RuntimeError("%s not found" % name)

                # Here's the whole point:  add the extension with sources
                exts.append(Extension(name, srcs + extra_srcs, **kwds))

            # Core Foundation
            core_kwds = {'extra_compile_args': carbon_extra_compile_args,
                         'extra_link_args': ['-framework', 'CoreFoundation'],
                        }
            addMacExtension('_CF', core_kwds, ['cf/pycfbridge.c'])
            addMacExtension('autoGIL', core_kwds)



            # Carbon
            carbon_kwds = {'extra_compile_args': carbon_extra_compile_args,
                           'extra_link_args': ['-framework', 'Carbon'],
                          }
            CARBON_EXTS = ['ColorPicker', 'gestalt', 'MacOS', 'Nav',
                           'OSATerminology', 'icglue',
                           # All these are in subdirs
                           '_AE', '_AH', '_App', '_CarbonEvt', '_Cm', '_Ctl',
                           '_Dlg', '_Drag', '_Evt', '_File', '_Folder', '_Fm',
                           '_Help', '_Icn', '_IBCarbon', '_List',
                           '_Menu', '_Mlte', '_OSA', '_Res', '_Qd', '_Qdoffs',
                           '_Scrap', '_Snd', '_TE',
                          ]
            for name in CARBON_EXTS:
                addMacExtension(name, carbon_kwds)

            # Workaround for a bug in the version of gcc shipped with Xcode 3.
            # The _Win extension should build just like the other Carbon extensions, but
            # this actually results in a hard crash of the linker.
            #
            if '-arch ppc64' in cflags and '-arch ppc' in cflags:
                win_kwds = {'extra_compile_args': carbon_extra_compile_args + ['-arch', 'i386', '-arch', 'ppc'],
                               'extra_link_args': ['-framework', 'Carbon', '-arch', 'i386', '-arch', 'ppc'],
                           }
                addMacExtension('_Win', win_kwds)
            else:
                addMacExtension('_Win', carbon_kwds)


            # Application Services & QuickTime
            app_kwds = {'extra_compile_args': carbon_extra_compile_args,
                        'extra_link_args': ['-framework','ApplicationServices'],
                       }
            addMacExtension('_Launch', app_kwds)
            addMacExtension('_CG', app_kwds)

            exts.append( Extension('_Qt', ['qt/_Qtmodule.c'],
                        extra_compile_args=carbon_extra_compile_args,
                        extra_link_args=['-framework', 'QuickTime',
                                     '-framework', 'Carbon']) )


        self.extensions.extend(exts)
        # Pyston change: disbale the tk module
        # # Call the method for detecting whether _tkinter can be compiled
        # self.detect_tkinter(inc_dirs, lib_dirs)
        #
        # if '_tkinter' not in [e.name for e in self.extensions]:
        #     missing.append('_tkinter')

##         # Uncomment these lines if you want to play with xxmodule.c
##         ext = Extension('xx', ['xxmodule.c'])
##         self.extensions.append(ext)

        return missing

    def detect_tkinter_explicitly(self):
        # Build _tkinter using explicit locations for Tcl/Tk.
        #
        # This is enabled when both arguments are given to ./configure:
        #
        #     --with-tcltk-includes="-I/path/to/tclincludes \
        #                            -I/path/to/tkincludes"
        #     --with-tcltk-libs="-L/path/to/tcllibs -ltclm.n \
        #                        -L/path/to/tklibs -ltkm.n"
        #
        # These values can also be specified or overriden via make:
        #    make TCLTK_INCLUDES="..." TCLTK_LIBS="..."
        #
        # This can be useful for building and testing tkinter with multiple
        # versions of Tcl/Tk.  Note that a build of Tk depends on a particular
        # build of Tcl so you need to specify both arguments and use care when
        # overriding.

        # The _TCLTK variables are created in the Makefile sharedmods target.
        tcltk_includes = os.environ.get('_TCLTK_INCLUDES')
        tcltk_libs = os.environ.get('_TCLTK_LIBS')
        if not (tcltk_includes and tcltk_libs):
            # Resume default configuration search.
            return 0

        extra_compile_args = tcltk_includes.split()
        extra_link_args = tcltk_libs.split()
        ext = Extension('_tkinter', ['_tkinter.c', 'tkappinit.c'],
                        define_macros=[('WITH_APPINIT', 1)],
                        extra_compile_args = extra_compile_args,
                        extra_link_args = extra_link_args,
                        )
        self.extensions.append(ext)
        return 1

    def detect_tkinter_darwin(self, inc_dirs, lib_dirs):
        # The _tkinter module, using frameworks. Since frameworks are quite
        # different the UNIX search logic is not sharable.
        from os.path import join, exists
        framework_dirs = [
            '/Library/Frameworks',
            '/System/Library/Frameworks/',
            join(os.getenv('HOME'), '/Library/Frameworks')
        ]

        sysroot = macosx_sdk_root()

        # Find the directory that contains the Tcl.framework and Tk.framework
        # bundles.
        # XXX distutils should support -F!
        for F in framework_dirs:
            # both Tcl.framework and Tk.framework should be present


            for fw in 'Tcl', 'Tk':
                if is_macosx_sdk_path(F):
                    if not exists(join(sysroot, F[1:], fw + '.framework')):
                        break
                else:
                    if not exists(join(F, fw + '.framework')):
                        break
            else:
                # ok, F is now directory with both frameworks. Continure
                # building
                break
        else:
            # Tk and Tcl frameworks not found. Normal "unix" tkinter search
            # will now resume.
            return 0

        # For 8.4a2, we must add -I options that point inside the Tcl and Tk
        # frameworks. In later release we should hopefully be able to pass
        # the -F option to gcc, which specifies a framework lookup path.
        #
        include_dirs = [
            join(F, fw + '.framework', H)
            for fw in 'Tcl', 'Tk'
            for H in 'Headers', 'Versions/Current/PrivateHeaders'
        ]

        # For 8.4a2, the X11 headers are not included. Rather than include a
        # complicated search, this is a hard-coded path. It could bail out
        # if X11 libs are not found...
        include_dirs.append('/usr/X11R6/include')
        frameworks = ['-framework', 'Tcl', '-framework', 'Tk']

        # All existing framework builds of Tcl/Tk don't support 64-bit
        # architectures.
        cflags = sysconfig.get_config_vars('CFLAGS')[0]
        archs = re.findall('-arch\s+(\w+)', cflags)

        if is_macosx_sdk_path(F):
            fp = os.popen("file %s/Tk.framework/Tk | grep 'for architecture'"%(os.path.join(sysroot, F[1:]),))
        else:
            fp = os.popen("file %s/Tk.framework/Tk | grep 'for architecture'"%(F,))

        detected_archs = []
        for ln in fp:
            a = ln.split()[-1]
            if a in archs:
                detected_archs.append(ln.split()[-1])
        fp.close()

        for a in detected_archs:
            frameworks.append('-arch')
            frameworks.append(a)

        ext = Extension('_tkinter', ['_tkinter.c', 'tkappinit.c'],
                        define_macros=[('WITH_APPINIT', 1)],
                        include_dirs = include_dirs,
                        libraries = [],
                        extra_compile_args = frameworks[2:],
                        extra_link_args = frameworks,
                        )
        self.extensions.append(ext)
        return 1

    def detect_tkinter(self, inc_dirs, lib_dirs):
        # The _tkinter module.

        # Check whether --with-tcltk-includes and --with-tcltk-libs were
        # configured or passed into the make target.  If so, use these values
        # to build tkinter and bypass the searches for Tcl and TK in standard
        # locations.
        if self.detect_tkinter_explicitly():
            return

        # Rather than complicate the code below, detecting and building
        # AquaTk is a separate method. Only one Tkinter will be built on
        # Darwin - either AquaTk, if it is found, or X11 based Tk.
        if (host_platform == 'darwin' and
            self.detect_tkinter_darwin(inc_dirs, lib_dirs)):
            return

        # Assume we haven't found any of the libraries or include files
        # The versions with dots are used on Unix, and the versions without
        # dots on Windows, for detection by cygwin.
        tcllib = tklib = tcl_includes = tk_includes = None
        for version in ['8.6', '86', '8.5', '85', '8.4', '84', '8.3', '83',
                        '8.2', '82', '8.1', '81', '8.0', '80']:
            tklib = self.compiler.find_library_file(lib_dirs,
                                                        'tk' + version)
            tcllib = self.compiler.find_library_file(lib_dirs,
                                                         'tcl' + version)
            if tklib and tcllib:
                # Exit the loop when we've found the Tcl/Tk libraries
                break

        # Now check for the header files
        if tklib and tcllib:
            # Check for the include files on Debian and {Free,Open}BSD, where
            # they're put in /usr/include/{tcl,tk}X.Y
            dotversion = version
            if '.' not in dotversion and "bsd" in host_platform.lower():
                # OpenBSD and FreeBSD use Tcl/Tk library names like libtcl83.a,
                # but the include subdirs are named like .../include/tcl8.3.
                dotversion = dotversion[:-1] + '.' + dotversion[-1]
            tcl_include_sub = []
            tk_include_sub = []
            for dir in inc_dirs:
                tcl_include_sub += [dir + os.sep + "tcl" + dotversion]
                tk_include_sub += [dir + os.sep + "tk" + dotversion]
            tk_include_sub += tcl_include_sub
            tcl_includes = find_file('tcl.h', inc_dirs, tcl_include_sub)
            tk_includes = find_file('tk.h', inc_dirs, tk_include_sub)

        if (tcllib is None or tklib is None or
            tcl_includes is None or tk_includes is None):
            self.announce("INFO: Can't locate Tcl/Tk libs and/or headers", 2)
            return

        # OK... everything seems to be present for Tcl/Tk.

        include_dirs = [] ; libs = [] ; defs = [] ; added_lib_dirs = []
        for dir in tcl_includes + tk_includes:
            if dir not in include_dirs:
                include_dirs.append(dir)

        # Check for various platform-specific directories
        if host_platform == 'sunos5':
            include_dirs.append('/usr/openwin/include')
            added_lib_dirs.append('/usr/openwin/lib')
        elif os.path.exists('/usr/X11R6/include'):
            include_dirs.append('/usr/X11R6/include')
            added_lib_dirs.append('/usr/X11R6/lib64')
            added_lib_dirs.append('/usr/X11R6/lib')
        elif os.path.exists('/usr/X11R5/include'):
            include_dirs.append('/usr/X11R5/include')
            added_lib_dirs.append('/usr/X11R5/lib')
        else:
            # Assume default location for X11
            include_dirs.append('/usr/X11/include')
            added_lib_dirs.append('/usr/X11/lib')

        # If Cygwin, then verify that X is installed before proceeding
        if host_platform == 'cygwin':
            x11_inc = find_file('X11/Xlib.h', [], include_dirs)
            if x11_inc is None:
                return

        # Check for BLT extension
        if self.compiler.find_library_file(lib_dirs + added_lib_dirs,
                                               'BLT8.0'):
            defs.append( ('WITH_BLT', 1) )
            libs.append('BLT8.0')
        elif self.compiler.find_library_file(lib_dirs + added_lib_dirs,
                                                'BLT'):
            defs.append( ('WITH_BLT', 1) )
            libs.append('BLT')

        # Add the Tcl/Tk libraries
        libs.append('tk'+ version)
        libs.append('tcl'+ version)

        if host_platform in ['aix3', 'aix4']:
            libs.append('ld')

        # Finally, link with the X11 libraries (not appropriate on cygwin)
        if host_platform != "cygwin":
            libs.append('X11')

        ext = Extension('_tkinter', ['_tkinter.c', 'tkappinit.c'],
                        define_macros=[('WITH_APPINIT', 1)] + defs,
                        include_dirs = include_dirs,
                        libraries = libs,
                        library_dirs = added_lib_dirs,
                        )
        self.extensions.append(ext)

        # XXX handle these, but how to detect?
        # *** Uncomment and edit for PIL (TkImaging) extension only:
        #       -DWITH_PIL -I../Extensions/Imaging/libImaging  tkImaging.c \
        # *** Uncomment and edit for TOGL extension only:
        #       -DWITH_TOGL togl.c \
        # *** Uncomment these for TOGL extension only:
        #       -lGL -lGLU -lXext -lXmu \

    def configure_ctypes_darwin(self, ext):
        # Darwin (OS X) uses preconfigured files, in
        # the Modules/_ctypes/libffi_osx directory.
        srcdir = sysconfig.get_config_var('srcdir')
        ffi_srcdir = os.path.abspath(os.path.join(srcdir, 'Modules',
                                                  '_ctypes', 'libffi_osx'))
        sources = [os.path.join(ffi_srcdir, p)
                   for p in ['ffi.c',
                             'x86/darwin64.S',
                             'x86/x86-darwin.S',
                             'x86/x86-ffi_darwin.c',
                             'x86/x86-ffi64.c',
                             'powerpc/ppc-darwin.S',
                             'powerpc/ppc-darwin_closure.S',
                             'powerpc/ppc-ffi_darwin.c',
                             'powerpc/ppc64-darwin_closure.S',
                             ]]

        # Add .S (preprocessed assembly) to C compiler source extensions.
        self.compiler.src_extensions.append('.S')

        include_dirs = [os.path.join(ffi_srcdir, 'include'),
                        os.path.join(ffi_srcdir, 'powerpc')]
        ext.include_dirs.extend(include_dirs)
        ext.sources.extend(sources)
        return True

    def configure_ctypes(self, ext):
        if not self.use_system_libffi:
            if host_platform == 'darwin':
                return self.configure_ctypes_darwin(ext)

            srcdir = sysconfig.get_config_var('srcdir')
            ffi_builddir = os.path.join(self.build_temp, 'libffi')
            ffi_srcdir = os.path.abspath(os.path.join(srcdir, 'Modules',
                                         '_ctypes', 'libffi'))
            ffi_configfile = os.path.join(ffi_builddir, 'fficonfig.py')

            from distutils.dep_util import newer_group

            config_sources = [os.path.join(ffi_srcdir, fname)
                              for fname in os.listdir(ffi_srcdir)
                              if os.path.isfile(os.path.join(ffi_srcdir, fname))]
            if self.force or newer_group(config_sources,
                                         ffi_configfile):
                from distutils.dir_util import mkpath
                mkpath(ffi_builddir)
                config_args = [arg for arg in sysconfig.get_config_var("CONFIG_ARGS").split()
                               if (('--host=' in arg) or ('--build=' in arg))]
                if not self.verbose:
                    config_args.append("-q")

                # Pass empty CFLAGS because we'll just append the resulting
                # CFLAGS to Python's; -g or -O2 is to be avoided.
                cmd = "cd %s && env CFLAGS='' '%s/configure' %s" \
                      % (ffi_builddir, ffi_srcdir, " ".join(config_args))

                res = os.system(cmd)
                if res or not os.path.exists(ffi_configfile):
                    print "Failed to configure _ctypes module"
                    return False

            fficonfig = {}
            with open(ffi_configfile) as f:
                exec f in fficonfig

            # Add .S (preprocessed assembly) to C compiler source extensions.
            self.compiler.src_extensions.append('.S')

            include_dirs = [os.path.join(ffi_builddir, 'include'),
                            ffi_builddir,
                            os.path.join(ffi_srcdir, 'src')]
            extra_compile_args = fficonfig['ffi_cflags'].split()

            ext.sources.extend(os.path.join(ffi_srcdir, f) for f in
                               fficonfig['ffi_sources'])
            ext.include_dirs.extend(include_dirs)
            ext.extra_compile_args.extend(extra_compile_args)
        return True

    def detect_ctypes(self, inc_dirs, lib_dirs):
        self.use_system_libffi = False
        include_dirs = []
        extra_compile_args = []
        extra_link_args = []
        sources = map(relpath, ["Modules/_ctypes/_ctypes.c",
                                "Modules/_ctypes/callbacks.c",
                                "Modules/_ctypes/callproc.c",
                                "Modules/_ctypes/stgdict.c",
                                "Modules/_ctypes/cfield.c"
                                ])
        depends = ['_ctypes/ctypes.h']

        if host_platform == 'darwin':
            sources.append('_ctypes/malloc_closure.c')
            sources.append('_ctypes/darwin/dlfcn_simple.c')
            extra_compile_args.append('-DMACOSX')
            include_dirs.append('_ctypes/darwin')
# XXX Is this still needed?
##            extra_link_args.extend(['-read_only_relocs', 'warning'])

        elif host_platform == 'sunos5':
            # XXX This shouldn't be necessary; it appears that some
            # of the assembler code is non-PIC (i.e. it has relocations
            # when it shouldn't. The proper fix would be to rewrite
            # the assembler code to be PIC.
            # This only works with GCC; the Sun compiler likely refuses
            # this option. If you want to compile ctypes with the Sun
            # compiler, please research a proper solution, instead of
            # finding some -z option for the Sun compiler.
            extra_link_args.append('-mimpure-text')

        elif host_platform.startswith('hp-ux'):
            extra_link_args.append('-fPIC')

        ext = Extension('_ctypes',
                        include_dirs=include_dirs,
                        extra_compile_args=extra_compile_args,
                        extra_link_args=extra_link_args,
                        libraries=[],
                        sources=sources,
                        depends=depends)
        ext_test = Extension('_ctypes_test',
                             sources= map(relpath, ['Modules/_ctypes/_ctypes_test.c']))
        self.extensions.extend([ext, ext_test])

        # Pyston change: Pyston don't support CONFIG_ARGS yet.
        # if not '--with-system-ffi' in sysconfig.get_config_var("CONFIG_ARGS"):
        #     return

        if host_platform == 'darwin':
            # OS X 10.5 comes with libffi.dylib; the include files are
            # in /usr/include/ffi
            inc_dirs.append('/usr/include/ffi')

        # Pyston change: still hard code the ffi include dir
        # because we don't support this variable configuration in get_config_var yet
        ffi_inc = ['/usr/include/x86_64-linux-gnu']
        # ffi_inc = [sysconfig.get_config_var("LIBFFI_INCLUDEDIR")]
        if not ffi_inc or ffi_inc[0] == '':
            ffi_inc = find_file('ffi.h', [], inc_dirs)
        if ffi_inc is not None:
            ffi_h = ffi_inc[0] + '/ffi.h'
            fp = open(ffi_h)
            while 1:
                line = fp.readline()
                if not line:
                    ffi_inc = None
                    break
                if line.startswith('#define LIBFFI_H'):
                    break
        ffi_lib = None
        if ffi_inc is not None:
            for lib_name in ('ffi_convenience', 'ffi_pic', 'ffi'):
                if (self.compiler.find_library_file(lib_dirs, lib_name)):
                    ffi_lib = lib_name
                    break

        if ffi_inc and ffi_lib:
            ext.include_dirs.extend(ffi_inc)
            ext.libraries.append(ffi_lib)
            self.use_system_libffi = True


class PyBuildInstall(install):
    # Suppress the warning about installation into the lib_dynload
    # directory, which is not in sys.path when running Python during
    # installation:
    def initialize_options (self):
        install.initialize_options(self)
        self.warn_dir=0

class PyBuildInstallLib(install_lib):
    # Do exactly what install_lib does but make sure correct access modes get
    # set on installed directories and files. All installed files with get
    # mode 644 unless they are a shared library in which case they will get
    # mode 755. All installed directories will get mode 755.

    so_ext = sysconfig.get_config_var("SO")

    def install(self):
        outfiles = install_lib.install(self)
        self.set_file_modes(outfiles, 0644, 0755)
        self.set_dir_modes(self.install_dir, 0755)
        return outfiles

    def set_file_modes(self, files, defaultMode, sharedLibMode):
        if not self.is_chmod_supported(): return
        if not files: return

        for filename in files:
            if os.path.islink(filename): continue
            mode = defaultMode
            if filename.endswith(self.so_ext): mode = sharedLibMode
            log.info("changing mode of %s to %o", filename, mode)
            if not self.dry_run: os.chmod(filename, mode)

    def set_dir_modes(self, dirname, mode):
        if not self.is_chmod_supported(): return
        os.path.walk(dirname, self.set_dir_modes_visitor, mode)

    def set_dir_modes_visitor(self, mode, dirname, names):
        if os.path.islink(dirname): return
        log.info("changing mode of %s to %o", dirname, mode)
        if not self.dry_run: os.chmod(dirname, mode)

    def is_chmod_supported(self):
        return hasattr(os, 'chmod')

SUMMARY = """
Python is an interpreted, interactive, object-oriented programming
language. It is often compared to Tcl, Perl, Scheme or Java.

Python combines remarkable power with very clear syntax. It has
modules, classes, exceptions, very high level dynamic data types, and
dynamic typing. There are interfaces to many system calls and
libraries, as well as to various windowing systems (X11, Motif, Tk,
Mac, MFC). New built-in modules are easily written in C or C++. Python
is also usable as an extension language for applications that need a
programmable interface.

The Python implementation is portable: it runs on many brands of UNIX,
on Windows, DOS, OS/2, Mac, Amiga... If your favorite system isn't
listed here, it may still be supported, if there's a C compiler for
it. Ask around on comp.lang.python -- or just try compiling Python
yourself.
"""

CLASSIFIERS = """
Development Status :: 6 - Mature
License :: OSI Approved :: Python Software Foundation License
Natural Language :: English
Programming Language :: C
Programming Language :: Python
Topic :: Software Development
"""

def main():
    # turn off warnings when deprecated modules are imported
    import warnings
    warnings.filterwarnings("ignore",category=DeprecationWarning)
    setup(
          name = "Pyston",
          version = "1.0",
          description = "Pyston shared modules",
          # Build info
          cmdclass = {'build_ext':PyBuildExt,},
          # The struct module is defined here, because build_ext won't be
          # called unless there's at least one extension module defined.
          ext_modules = [Extension("_struct", sources = map(relpath, [
              "Modules/_struct.c",
          ]))]

        )

# --install-platlib
if __name__ == '__main__':
    main()
