/*
 * Copyright (c) 2010-2017 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "application.h"
#include <csignal>
#include <framework/core/clock.h>
#include <framework/core/resourcemanager.h>
#include <framework/core/modulemanager.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/configmanager.h>
#include "asyncdispatcher.h"
#include <framework/luaengine/luainterface.h>
#include <framework/platform/crashhandler.h>
#include <framework/platform/platform.h>
#include <framework/http/http.h>

#include <locale>

#if defined(WIN32)
#include <windows.h>
#include <filesystem>
#include <vector>
#endif

#include <framework/net/connection.h>
#include <framework/proxy/proxy.h>

#if defined(WIN32)
namespace {
// Quote Windows command-line arguments following the CommandLineToArgvW rules.
std::wstring quoteWindowsArg(const std::wstring& argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool launchReplacement(const std::string& binaryName, const std::vector<std::string>& args)
{
    const std::filesystem::path path = std::filesystem::u8path(binaryName);
    const std::wstring executable = path.wstring();
    std::wstring commandLine = quoteWindowsArg(executable);
    for (const auto& arg : args) {
        const std::filesystem::path argPath = std::filesystem::u8path(arg);
        commandLine += L" " + quoteWindowsArg(argPath.wstring());
    }

    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    if (!CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startupInfo, &processInfo)) {
        return false;
    }

    CloseHandle(processInfo.hThread);
    const DWORD result = WaitForSingleObject(processInfo.hProcess, 1000);
    CloseHandle(processInfo.hProcess);
    return result == WAIT_TIMEOUT;
}
} // namespace
#endif

void exitSignalHandler(int sig)
{
    static bool signaled = false;
    switch(sig) {
        case SIGTERM:
        case SIGINT:
            if(!signaled && !g_app.isStopping() && !g_app.isTerminated()) {
                signaled = true;
                g_dispatcher.addEvent(std::bind(&Application::close, &g_app));
            }
            break;
    }
}

Application::Application()
{
    m_appName = "application";
    m_appCompactName = "app";
    m_appVersion = "none";
    m_charset = "cp1252";
    m_stopping = false;
#ifdef ANDROID
    m_mobile = true;
#endif
}

void Application::init(std::vector<std::string>& args)
{
    // capture exit signals
    signal(SIGTERM, exitSignalHandler);
    signal(SIGINT, exitSignalHandler);

    // setup locale
    std::locale::global(std::locale());

    // process args encoding
    g_platform.processArgs(args);

    g_asyncDispatcher.init();

    std::string startupOptions;
    for(uint i=1;i<args.size();++i) {
        const std::string& arg = args[i];
        startupOptions += " ";
        startupOptions += arg;
    }
    if(startupOptions.length() > 0)
        g_logger.info(stdext::format("Startup options: %s", startupOptions));

    m_startupOptions = startupOptions;

    // mobile testing
    if (startupOptions.find("-mobile") != std::string::npos)
        m_mobile = true;

    // initialize configs
    g_configs.init();

    // initialize lua
    g_lua.init();
    registerLuaFunctions();

    // initalize proxy
    g_proxy.init();
}

void Application::deinit()
{
    g_lua.callGlobalField("g_app", "onTerminate");

    // run modules unload events
    g_modules.unloadModules();
    g_modules.clear();

    // release remaining lua object references
    g_lua.collectGarbage();

    // poll remaining events
    poll();

    // disable dispatcher events
    g_dispatcher.shutdown();
}

void Application::terminate()
{
    // terminate network
    Connection::terminate();

    // release configs
    g_configs.terminate();

    // release resources
    g_resources.terminate();

    // terminate script environment
    g_lua.terminate();

    // terminate proxy
    g_proxy.terminate();

    m_terminated = true;

    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
}

void Application::poll()
{
    Connection::poll();

    g_dispatcher.poll();

    // poll connection again to flush pending write
    Connection::poll();
}

void Application::exit()
{
    g_lua.callGlobalField<bool>("g_app", "onExit");
    m_stopping = true;
}

void Application::quick_exit()
{
#ifdef _MSC_VER
    ::quick_exit(0);
#else
    ::exit(0);
#endif
}

void Application::close()
{
    if(!g_lua.callGlobalField<bool>("g_app", "onClose"))
        exit();
}

void Application::restart()
{
#if defined(WIN32) && !defined(FREE_VERSION)
    if (!launchReplacement(g_resources.getBinaryName(), {})) {
        g_logger.fatal("Updater restart error. Please restart application");
    }
    quick_exit();
#else
    exit();
#endif
}

void Application::restartArgs(const std::vector<std::string>& args)
{
#if defined(WIN32) && !defined(FREE_VERSION)
    if (!launchReplacement(g_resources.getBinaryName(), args)) {
        g_logger.fatal("Updater restart error. Please restart application");
    }
    quick_exit();
#else
    exit();
#endif
}

std::string Application::getOs()
{
#if defined(ANDROID)
    return "android";
#elif defined(WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#elif __linux
    return "linux";
#else
    return "unknown";
#endif
}
