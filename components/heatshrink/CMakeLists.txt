# Unmodified upstream 7d419e1fa4830d0b919b9b6a91fe2fb786cf3280.
# https://github.com/atomicobject/heatshrink (ISC license in LICENSE)
idf_component_register(SRCS "heatshrink_encoder.c" "heatshrink_decoder.c" INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PUBLIC HEATSHRINK_DYNAMIC_ALLOC=0)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
