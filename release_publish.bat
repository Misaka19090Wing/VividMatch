@echo off
rem Publishes the v0.1.0 release: pushes the tag, then creates the GitHub release
rem with the portable zip attached.
rem
rem Needs a GitHub token with "repo" scope. Create one at
rem https://github.com/settings/tokens and set it first:
rem     set GITHUB_TOKEN=ghp_xxx
rem     release_publish.bat
rem
rem This is the one step that cannot be done from inside the agent sandbox, which
rem has no GitHub credentials.

setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if "%GITHUB_TOKEN%"=="" (
    echo [release_publish] GITHUB_TOKEN is not set.
    echo [release_publish] Create a token with "repo" scope, then:
    echo [release_publish]     set GITHUB_TOKEN=ghp_xxx
    exit /b 1
)

echo [release_publish] pushing tag v0.1.0...
git -C "%ROOT%" push origin v0.1.0
if errorlevel 1 exit /b 1

python "%ROOT%\tests\release\publish_release.py"
if errorlevel 1 exit /b 1

endlocal
