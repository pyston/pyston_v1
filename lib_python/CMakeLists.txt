# Copy any changed stdlib files to the destination:
file(GLOB_RECURSE STDLIB_SRCS 2.7/ "*.py")
SET(STDLIB_TARGETS "")
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

# compile specified files in lib_python/2.7_Modules
file(GLOB_RECURSE STDMODULE_SRCS 2.7_Modules errnomodule.c shamodule.c sha256module.c sha512module.c _math.c mathmodule.c md5.c md5module.c _randommodule.c _sre.c operator.c binascii.c pwdmodule.c posixmodule.c)

# compile specified files in lib_python/2.7_Objects
file(GLOB_RECURSE STDOBJECT_SRCS 2.7_Objects structseq.c capsule.c)

# compile specified files in lib_python/2.7_Python
file(GLOB_RECURSE STDPYTHON_SRCS 2.7_Python getargs.c pyctype.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits")
add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS} ${STDPYTHON_SRCS})
