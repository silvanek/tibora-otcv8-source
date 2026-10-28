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

#include "resourcemanager.h"
#include "filestream.h"
#include "resource.h"

#include <framework/core/application.h>
#include <framework/luaengine/luainterface.h>
#include <framework/platform/platform.h>
#include <framework/util/crypt.h>
#include <framework/http/http.h>
#include <queue>
#include <regex>

#if defined(WIN32)
#include <windows.h>
#endif
#include <locale>
#include <zlib.h>

#define PHYSFS_DEPRECATED
#include <physfs.h>
#ifndef __EMSCRIPTEN__
#include <zip.h>
#include <zlib.h>
#endif

ResourceManager g_resources;
static const std::string INIT_FILENAME = "init.lua";

void ResourceManager::init(const char *argv0)
{
#if defined(WIN32)
    char fileName[255];
    GetModuleFileNameA(NULL, fileName, sizeof(fileName));
    m_binaryPath = std::filesystem::absolute(fileName);
#elif defined(ANDROID)
    // nothing
#else
    m_binaryPath = std::filesystem::absolute(argv0);    
#endif
    PHYSFS_init(argv0);
    PHYSFS_permitSymbolicLinks(1);
}

void ResourceManager::terminate()
{
    PHYSFS_deinit();
}

bool ResourceManager::launchCorrect(const std::string& product, const std::string& app) { // curently works only on windows
#if not(defined(ANDROID) || defined(FREE_VERSION))
    auto init_path = m_binaryPath.parent_path();
    init_path /= INIT_FILENAME;
    if (std::filesystem::exists(init_path)) // debug version
        return false;

    const char* localDir = PHYSFS_getPrefDir(product.c_str(), app.c_str());
    if (!localDir)
        return false;

    auto fileName2 = m_binaryPath.stem().string();
    fileName2 = stdext::split(fileName2, "-")[0];
    stdext::tolower(fileName2);

    std::filesystem::path path(std::filesystem::u8path(localDir));
    std::error_code ec;
    auto lastWrite = std::filesystem::last_write_time(m_binaryPath, ec);
    std::filesystem::path binary = m_binaryPath;
    for (auto& entry : std::filesystem::directory_iterator(path)) {
        if (std::filesystem::is_directory(entry.path()))
            continue;

        auto fileName1 = entry.path().stem().string();
        fileName1 = stdext::split(fileName1, "-")[0];
        stdext::tolower(fileName1);
