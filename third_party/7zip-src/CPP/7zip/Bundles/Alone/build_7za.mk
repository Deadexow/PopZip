CC = D:/Code/Gcc/bin/gcc.exe -B D:/Code/Gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/ -B D:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0/ -B D:/Code/Gcc/x86_64-w64-mingw32/lib/ -isystem D:/Code/Gcc/x86_64-w64-mingw32/include
CXX = D:/Code/Gcc/bin/g++.exe -B D:/Code/Gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/ -B D:/Code/Gcc/lib/gcc/x86_64-w64-mingw32/16.2.0/ -B D:/Code/Gcc/x86_64-w64-mingw32/lib/ -isystem D:/Code/Gcc/include/c++/16.2.0 -isystem D:/Code/Gcc/include/c++/16.2.0/x86_64-w64-mingw32 -isystem D:/Code/Gcc/x86_64-w64-mingw32/include -L D:/Code/Gcc/lib

include makefile.gcc

CFLAGS_WARN_WALL =

_o/resource.o: _o/resource_placeholder.c
	$(CC) $(CFLAGS) -o $@ $<