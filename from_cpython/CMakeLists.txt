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
    connection.c
    cStringIO.c
    cursor.c
    datetimemodule.c
    errnomodule.c
    fcntlmodule.c
    fileio.c
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
    textio.c
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
    exceptions.c
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
    dtoa.c
    formatter_string.c
    formatter_unicode.c
    getargs.c
    marshal.c
    pyctype.c
    pystrtod.c
    structmember.c
    )

# compile specified files in from_cpython/Python
file(GLOB_RECURSE STDPARSER_SRCS Parser
    myreadline.c
    )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits -Wno-unused-result -Wno-strict-aliasing")
add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS} ${STDPYTHON_SRCS} ${STDPARSER_SRCS})
