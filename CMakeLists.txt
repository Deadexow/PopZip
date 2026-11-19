cmake_minimum_required(VERSION 3.20)
set(CMAKE_RC_FLAGS "--preprocessor-arg=-BD:/Code/Gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/")
project(PopZip VERSION 1.58.0 LANGUAGES C CXX RC)

get_filename_component(_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
set(CMAKE_OBJCOPY "${_compiler_dir}/objcopy.exe")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/theme_extreme.o"
    COMMAND "${CMAKE_OBJCOPY}" -I binary -O pe-x86-64 -B i386:x86-64
            extreme.jpg "${CMAKE_CURRENT_BINARY_DIR}/theme_extreme.o"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/extreme.jpg"
    VERBATIM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(PopZip WIN32
    src/main.cpp
    src/password_dialog.cpp
    src/sevenzip_engine.cpp
    src/zpaq_engine.cpp
    src/compress_engine.cpp
    src/compress_dialog.cpp
    src/preview_dialog.cpp
    src/settings_dialog.cpp
    src/conflict_dialog.cpp
    src/confirm_dialog.cpp
    src/rename_dialog.cpp
    src/eula_dialog.cpp
    src/shell_integration.cpp
    src/sky.cpp
    popzip.rc

    "${CMAKE_CURRENT_BINARY_DIR}/theme_extreme.o"


    third_party/zpaq/zpaq.cpp
    third_party/zpaq/libzpaq.cpp
)

target_include_directories(PopZip PRIVATE 
    third_party/zpaq
    third_party/7zip-src/CPP
    third_party/7zip-src/CPP/7zip
    third_party/7zip-src/CPP/7zip/Common
    third_party/7zip-src/CPP/Common
    third_party/7zip-src/CPP/Windows
)

target_compile_definitions(PopZip PRIVATE
    UNICODE
    _UNICODE

    _7ZIP_ST  # 非常重要：启用单线程版本
)

target_link_libraries(PopZip PRIVATE
    "-Wl,--whole-archive"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/7zip/lib7zengine.a"
    "-Wl,--no-whole-archive"
    oleaut32
    uuid
    advapi32
    ole32
    comdlg32
    htmlhelp
    dwmapi
    comctl32
    shell32
    gdi32
    msimg32
    user32
    mfplay
    mfplat
    mf
    mfuuid
    windowscodecs
)

if(MINGW)
    target_compile_options(PopZip PRIVATE
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(PopZip PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -mwindows
        -Wl,--gc-sections
        -s
    )
endif()