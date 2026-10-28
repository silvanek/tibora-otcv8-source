$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$url = 'https://raw.githubusercontent.com/silvanek/tibora-otcv8-source/master/src/framework/core/resourcemanager.cpp'
$source = (Invoke-WebRequest -Uri $url -UseBasicParsing).Content
if ($source -is [byte[]]) { $source = [Text.Encoding]::UTF8.GetString($source) }
$expected = @'
#if not(defined(ANDROID) || defined(FREE_VERSION))
#include <boost/process.hpp>
#endif
'@
if (-not $source.Contains($expected)) { throw 'Plik z GitHuba zmienil sie - nie wykonano podmiany.' }
$old = @'
    boost::process::child c(binary.string());
    std::error_code ec2;
    if (c.wait_for(std::chrono::seconds(5), ec2)) {
        return c.exit_code() == 0;
    }

    c.detach();
    return true;
'@
if (-not $source.Contains($old)) { throw 'Nie znaleziono oryginalnego bloku Boost.Process - przerwano.' }
$header = @'
#if defined(WIN32)
#include <windows.h>
#endif
'@
$new = @'
#if defined(WIN32)
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};

    std::wstring commandLine = L"\"" + binary.wstring() + L"\"";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');

    const BOOL started = CreateProcessW(
        binary.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE,
        0, nullptr, nullptr, &startupInfo, &processInfo);
    if (!started)
        return false;

    CloseHandle(processInfo.hThread);
    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 5000);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD exitCode = 1;
        const BOOL gotExitCode = GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hProcess);
        return gotExitCode && exitCode == 0;
    }
    CloseHandle(processInfo.hProcess);
    return waitResult == WAIT_TIMEOUT;
#else
    return false;
#endif
'@
$source = $source.Replace($expected, $header).Replace($old, $new)
$path = Join-Path (Get-Location) 'resourcemanager.cpp'
[System.IO.File]::WriteAllText($path, $source, (New-Object System.Text.UTF8Encoding($false)))
Set-Clipboard -Value $source
Write-Host "Gotowe: $path" -ForegroundColor Green
Write-Host "Pelny poprawiony kod skopiowano do schowka. W edytorze GitHub uzyj Ctrl+A, Ctrl+V i Commit changes."
Write-Host 'Wgraj plik na GitHub do src/framework/core/resourcemanager.cpp, zastępując poprzedni.'
