idf_component_register(SRCS "tjpgd.c" "feishu_jpeg.c" INCLUDE_DIRS ".")
# Keep the vendor API private so LVGL's optional JPEG decoder can coexist.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    jd_prepare=feishu_jd_prepare jd_decomp=feishu_jd_decomp)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
