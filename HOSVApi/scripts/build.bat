@echo off
setlocal

cmake -S . -B build -DSEEDANCE2_USE_BUNDLED_DEPS=ON
if errorlevel 1 exit /b %errorlevel%

cmake --build build --config Release
exit /b %errorlevel%

