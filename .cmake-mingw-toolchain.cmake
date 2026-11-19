set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "D:/Code/Gcc/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "D:/Code/Gcc/bin/g++.exe")
set(CMAKE_RC_COMPILER "D:/Code/Gcc/bin/windres.exe")
set(CMAKE_AR "D:/Code/Gcc/bin/ar.exe")
set(CMAKE_RANLIB "D:/Code/Gcc/bin/ranlib.exe")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_mi_B "D:/Code/Gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/"
            "D:/Code/Gcc/x86_64-w64-mingw32/bin/"
            "D:/Code/Gcc/bin/")
set(_mi_is "D:/Code/Gcc/include/c++/16.2.0"
           "D:/Code/Gcc/include/c++/16.2.0/x86_64-w64-mingw32"
           "D:/Code/Gcc/include/c++/16.2.0/backward"
           "D:/Code/Gcc/x86_64-w64-mingw32/include"
           "D:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0/include")
set(_mi_flags "")
foreach(d ${_mi_B})
  string(APPEND _mi_flags " -B ${d}")
endforeach()
foreach(d ${_mi_is})
  string(APPEND _mi_flags " -isystem ${d}")
endforeach()
string(APPEND _mi_flags " -LD:/Code/Gcc/x86_64-w64-mingw32/lib -LD:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0 -LD:/Code/Gcc/lib")

set(CMAKE_C_FLAGS_INIT "${_mi_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mi_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT " -LD:/Code/Gcc/x86_64-w64-mingw32/lib -LD:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0 -LD:/Code/Gcc/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT " -LD:/Code/Gcc/x86_64-w64-mingw32/lib -LD:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0 -LD:/Code/Gcc/lib")