# compile specified files in lib_python/2.7_Modules
file(GLOB_RECURSE FROM_CPYTHON_SRCS errnomodule.c shamodule.c sha256module.c sha512module.c _math.c mathmodule.c md5.c md5module.c _randommodule.c _sre.c operator.c binascii.c)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers")
add_library(FROM_CPYTHON_OBJECTS OBJECT ${FROM_CPYTHON_SRCS})
