@echo off
rem ---------------------------------------------------------------------------
rem  MouseMux Chrome - start here
rem
rem  BEFORE running this: start your normal Chrome through MouseMux once, so
rem  MouseMux opens its input service, then close that Chrome again. This
rem  browser connects to that service.
rem
rem  This uses its own profile, in the "user-data" folder beside this file, so
rem  it never touches your installed Chrome. Sign in ONCE here and every user
rem  shares that session - which is the point: one login, several people.
rem
rem  To add users: press "+ Window" in the MouseMux dialog, once per person,
rem  and have each person click in their own window to claim it. Then press
rem  "Capture all".
rem ---------------------------------------------------------------------------

start "" "%~dp0chrome.exe" ^
  --enable-features=MouseMuxIntegration ^
  --mousemux-control-port=52001 ^
  --user-data-dir="%~dp0user-data" ^
  --no-first-run
