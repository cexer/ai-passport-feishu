# Unmodified MIT-licensed mnunberg/jsonsl revision:
# 684b60f9af68b8c397422e74d0c2dd206de16a2c (LICENSE retained).
idf_component_register(SRCS "jsonsl.c" INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PUBLIC JSONSL_NO_JPR JSONSL_STATE_GENERIC)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
