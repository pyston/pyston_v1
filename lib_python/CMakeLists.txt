# compile specified files in lib_python/2.7_Modules
file(GLOB_RECURSE STDMODULE_SRCS 2.7_Modules errnomodule.c shamodule.c sha256module.c sha512module.c _math.c mathmodule.c md5.c md5module.c _randommodule.c _sre.c operator.c binascii.c pwdmodule.c)

# compile specified files in lib_python/2.7_Objects
file(GLOB_RECURSE STDOBJECT_SRCS 2.7_Objects structseq.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits")
add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS})
