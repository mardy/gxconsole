set(PROG_NAME gxcon-example)

add_executable(${PROG_NAME}
    main.c
)

target_link_libraries(${PROG_NAME} PRIVATE
    gxconsole
)

ogc_create_dol(${PROG_NAME})
