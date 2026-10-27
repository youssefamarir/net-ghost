file(GLOB LIBSSH2_SRCS "src/*.c")
idf_component_register(
    SRCS ${LIBSSH2_SRCS}
    INCLUDE_DIRS "include" "src"
    REQUIRES mbedtls lwip
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIBSSH2_MBEDTLS=1 HAVE_CONFIG_H=1 LIBSSH2DEBUG=0)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error -w)
