
REM Run in either of the Visual Studio Command prompts (x86 or x64)

set BUILDDIR=build%Platform%
mkdir %BUILDDIR%
cl.exe /MT /W3 /c /Fo%BUILDDIR%\sqlite3.obj sqlite3.c
lib.exe /out:%BUILDDIR%\sqlite3.lib %BUILDDIR%\sqlite3.obj