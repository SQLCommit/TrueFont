cmake_minimum_required(VERSION 3.22)
project(truefont LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(AshitaSDK REQUIRED)

# FreeType 2.14.3 (the second engine), vendored in third_party/freetype and trimmed by third_party/freetype-config.
set(FT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype")
add_library(freetype_trim STATIC
    ${FT_DIR}/builds/windows/ftsystem.c ${FT_DIR}/builds/windows/ftdebug.c ${FT_DIR}/src/base/ftinit.c ${FT_DIR}/src/base/ftbase.c
    ${FT_DIR}/src/base/ftmm.c ${FT_DIR}/src/base/ftbitmap.c ${FT_DIR}/src/base/ftsynth.c ${FT_DIR}/src/autofit/autofit.c
    ${FT_DIR}/src/truetype/truetype.c ${FT_DIR}/src/cff/cff.c ${FT_DIR}/src/sfnt/sfnt.c ${FT_DIR}/src/psaux/psaux.c
    ${FT_DIR}/src/pshinter/pshinter.c ${FT_DIR}/src/psnames/psnames.c ${FT_DIR}/src/smooth/smooth.c)
target_include_directories(freetype_trim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-config" "${FT_DIR}/include")
target_compile_definitions(freetype_trim PRIVATE FT2_BUILD_LIBRARY _CRT_SECURE_NO_WARNINGS)
target_compile_options(freetype_trim PRIVATE /W4 /WX /fp:precise)
set_target_properties(freetype_trim PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The release build only: TF_DEV is never defined here.
add_library(truefont MODULE src/truefont.cpp src/truefont.def)
set_target_properties(truefont PROPERTIES
    PREFIX ""
    OUTPUT_NAME truefont
    SUFFIX ".dll"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

ashita_sdk_attach(truefont)
target_compile_options(truefont PRIVATE /EHa /W4 /WX /fp:precise)
target_link_libraries(truefont PRIVATE freetype_trim user32 gdi32 ole32)
target_link_options(truefont PRIVATE /MAP /MAPINFO:EXPORTS)
