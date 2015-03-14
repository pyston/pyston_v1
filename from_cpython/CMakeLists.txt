# Copy any changed stdlib files to the destination:
file(GLOB_RECURSE STDLIB_SRCS Lib/ "*.py")
set(STDLIB_TARGETS "")
foreach(STDLIB_FILE ${STDLIB_SRCS})
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
    errnomodule.c
    shamodule.c
    sha256module.c
    sha512module.c
    _math.c
    mathmodule.c
    md5.c
    md5module.c
    _randommodule.c
    _sre.c
    operator.c
    binascii.c
    pwdmodule.c
    posixmodule.c
    _struct.c
    datetimemodule.c
    _functoolsmodule.c
    _collectionsmodule.c
    itertoolsmodule.c
    resource.c
    signalmodule.c
    selectmodule.c
    fcntlmodule.c
    timemodule.c
    arraymodule.c
    zlibmodule.c
    _codecsmodule.c
    socketmodule.c
    unicodedata.c
    _weakref.c
    cStringIO.c
    bufferedio.c
    bytesio.c
    fileio.c
    iobase.c
    _iomodule.c
    stringio.c
    textio.c
    zipimport.c
    _csv.c
    _ssl.c
    getpath.c
    cache.c
    connection.c
    cursor.c
    microprotocols.c
    module.c
    prepare_protocol.c
    row.c
    statement.c
    util.c
    )

# compile specified files in from_cpython/Objects
file(GLOB_RECURSE STDOBJECT_SRCS Objects
    structseq.c
    capsule.c
    stringobject.c
    exceptions.c
    unicodeobject.c
    unicodectype.c
    bytearrayobject.c
    bytes_methods.c
    weakrefobject.c
    memoryobject.c
    iterobject.c
    bufferobject.c
    )

# compile specified files in from_cpython/Python
file(GLOB_RECURSE STDPYTHON_SRCS Python
    getargs.c
    pyctype.c
    formatter_string.c
    pystrtod.c
    dtoa.c
    formatter_unicode.c
    structmember.c
    marshal.c
    )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits -Wno-unused-result -Wno-strict-aliasing")
add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS} ${STDPYTHON_SRCS})
