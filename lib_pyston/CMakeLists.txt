# Copy any changed lib_pyston sources:
file(GLOB_RECURSE LIBPYSTON_SRCS . "*.py")
set(LIBPYSTON_TARGETS "")
foreach(STDLIB_FILE ${LIBPYSTON_SRCS})
    file(RELATIVE_PATH FN_REL ${CMAKE_SOURCE_DIR} ${STDLIB_FILE})
    set(TARGET ${CMAKE_BINARY_DIR}/${FN_REL})

    add_custom_command(OUTPUT ${TARGET} COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different ${STDLIB_FILE} ${TARGET}
                       DEPENDS ${STDLIB_FILE}
                       COMMENT "Copying ${FN_REL}"
                       )
    set(LIBPYSTON_TARGETS ${LIBPYSTON_TARGETS} ${TARGET})
endforeach(STDLIB_FILE)
add_custom_target(copy_libpyston ALL DEPENDS ${LIBPYSTON_TARGETS})

install(FILES ${LIBPYSTON_SRCS} DESTINATION lib_pyston)
