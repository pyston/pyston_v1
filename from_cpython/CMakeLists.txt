# Copy any changed stdlib files to the destination:
file(GLOB_RECURSE STDLIB_SRCS Lib/ "*.py")
file(GLOB_RECURSE STD_INCLUDES Include/ "*.h")
set(STDLIB_TARGETS "")
foreach(STDLIB_FILE ${STDLIB_SRCS} ${STD_INCLUDES})
    file(RELATIVE_PATH FN_REL ${CMAKE_SOURCE_DIR} ${STDLIB_FILE})
    set(TARGET ${CMAKE_BINARY_DIR}/${FN_REL})

    add_custom_command(OUTPUT ${TARGET} COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different ${STDLIB_FILE} ${TARGET}
                       DEPENDS ${STDLIB_FILE}
                       COMMENT "Copying ${FN_REL}"
                       )
    set(STDLIB_TARGETS ${STDLIB_TARGETS} ${TARGET})

    get_filename_component(DIR ${FN_REL} DIRECTORY)
    install(FILES ${STDLIB_FILE} DESTINATION ${DIR})
endforeach(STDLIB_FILE)
add_custom_target(copy_stdlib ALL DEPENDS ${STDLIB_TARGETS})

# compile specified files in from_cpython/Modules
file(GLOB_RECURSE STDMODULE_SRCS Modules
    _codecsmodule.c
    _collectionsmodule.c
    _csv.c
    _functoolsmodule.c
    _iomodule.c
    _math.c
    _randommodule.c
    _sre.c
    _ssl.c
    _struct.c
    _weakref.c
    arraymodule.c
    binascii.c
    bufferedio.c
    bytesio.c
    cache.c
    cmathmodule.c
    connection.c
    cStringIO.c
    cursor.c
    datetimemodule.c
    errnomodule.c
    fcntlmodule.c
    fileio.c
    future_builtins.c
    getpath.c
    iobase.c
    itertoolsmodule.c
    mathmodule.c
    md5.c
    md5module.c
    microprotocols.c
    module.c
    operator.c
    posixmodule.c
    prepare_protocol.c
    pwdmodule.c
    resource.c
    row.c
    selectmodule.c
    sha256module.c
    sha512module.c
    shamodule.c
    signalmodule.c
    socketmodule.c
    statement.c
    stringio.c
    stropmodule.c
    textio.c
    threadmodule.c
    timemodule.c
    unicodedata.c
    util.c
    zipimport.c
    zlibmodule.c
    )

# compile specified files in from_cpython/Objects
file(GLOB_RECURSE STDOBJECT_SRCS Objects
    bufferobject.c
    bytearrayobject.c
    bytes_methods.c
    capsule.c
    cobject.c
    complexobject.c
    dictobject.c
    dictproxy.c
    exceptions.c
    floatobject.c
    iterobject.c
    memoryobject.c
    stringobject.c
    structseq.c
    unicodectype.c
    unicodeobject.c
    weakrefobject.c
    )

# compile specified files in from_cpython/Python
file(GLOB_RECURSE STDPYTHON_SRCS Python
    asdl.c
    ast.c
    dtoa.c
    formatter_string.c
    formatter_unicode.c
    getargs.c
    graminit.c
    marshal.c
    mystrtoul.c
    pyarena.c
    pyctype.c
    pystrtod.c
    Python-ast.c
    random.c
    structmember.c
    _warnings.c
    )

# compile specified files in from_cpython/Python
file(GLOB_RECURSE STDPARSER_SRCS Parser
    acceler.c
    grammar1.c
    myreadline.c
    node.c
    parser.c
    parsetok.c
    tokenizer.c
    )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits -Wno-unused-result -Wno-strict-aliasing -DPy_BUILD_CORE")
add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS} ${STDPYTHON_SRCS} ${STDPARSER_SRCS})
add_dependencies(FROM_CPYTHON copy_stdlib)

set(STDMODULES
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/future_builtins.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/_multiprocessing.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/pyexpat.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/_elementtree.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/bz2.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/_ctypes.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/grp.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/readline.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/termios.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/_curses.pyston.so
    ${CMAKE_BINARY_DIR}/from_cpython/Lib/mmap.pyston.so
)

add_custom_command(OUTPUT ${STDMODULES}
    COMMAND ${CMAKE_BINARY_DIR}/pyston setup.py build --build-lib ${CMAKE_BINARY_DIR}/from_cpython/Lib
                   DEPENDS
                       pyston
                       copy_stdlib
                       copy_libpyston
                       Modules/_multiprocessing/multiprocessing.c
                       Modules/_multiprocessing/semaphore.c
                       Modules/_multiprocessing/socket_connection.c
                       Modules/_ctypes/_ctypes.c
                       Modules/_ctypes/callbacks.c
                       Modules/_ctypes/callproc.c
                       Modules/_ctypes/stgdict.c
                       Modules/_ctypes/cfield.c
                       Modules/expat/xmlparse.c
                       Modules/expat/xmlrole.c
                       Modules/expat/xmltok.c
                       Modules/expat/xmltok_impl.c
                       Modules/expat/xmltok_ns.c
                       Modules/pyexpat.c
                       Modules/future_builtins.c
                       Modules/_elementtree.c
		       Modules/bz2module.c
		       Modules/grpmodule.c
		       Modules/readline.c
		       Modules/termios.c
		       Modules/_cursesmodule.c
		       Modules/mmapmodule.c
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_target(sharedmods ALL DEPENDS ${CMAKE_BINARY_DIR}/from_cpython/Lib/_multiprocessing.pyston.so)

install(FILES ${STDMODULES} DESTINATION from_cpython/Lib)
