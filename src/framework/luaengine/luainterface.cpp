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

#include "luainterface.h"
#include <exception>
#include "luaobject.h"

#include <framework/core/resourcemanager.h>
#include <framework/util/stats.h>

extern "C" {
#if defined(_MSC_VER) || defined(ANDROID)
#include <luajit/lua.h>
#include <luajit/lualib.h>
#include <luajit/lauxlib.h>
#else
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#endif
}

#include "lbitlib.h"

LuaInterface g_lua;

LuaInterface::LuaInterface()
{
    L = nullptr;
    m_cppCallbackDepth = 0;
    m_weakTableRef = 0;
    m_totalObjRefs = 0;
    m_totalFuncRefs = 0;
}

LuaInterface::~LuaInterface()
{
}

void LuaInterface::init()
{
    createLuaState();

    // store global environment reference
    pushThread();
    getEnv();
    m_globalEnv = ref();
    pop();

    // check if demangle_class is working as expected
    VALIDATE(stdext::demangle_class<LuaObject>() == "LuaObject");

    // register LuaObject, the base of all other objects
    registerClass<LuaObject>();
    bindClassMemberFunction<LuaObject>("getUseCount", &LuaObject::getUseCount);
    bindClassMemberFunction<LuaObject>("getClassName", &LuaObject::getClassName);

    registerClassMemberFunction<LuaObject>("getFieldsTable", (LuaCppFunction) ([](LuaInterface* lua) -> int {
        LuaObjectPtr obj = g_lua.popObject();
        obj->luaGetFieldsTable();
        return 1;
    }));
}

void LuaInterface::terminate()
{
    // close lua state, it will release all objects
    closeLuaState();
    VALIDATE(m_totalFuncRefs == 0);
    VALIDATE(m_totalObjRefs == 0);
}

void LuaInterface::registerSingletonClass(const std::string& className)
{
    newTable();
    pushValue();
    setGlobal(className);
    pop();
}

void LuaInterface::registerClass(const std::string& className, const std::string& baseClass)
{
    newTable();
    pushValue();
    setGlobal(className);
    const int klass = getTop();

    // create class fieldmethods table
    newTable();
    pushValue();
    setGlobal(className + "_fieldmethods");
    int klass_fieldmethods = getTop();

    // create class metatable
    newTable();
    pushValue();
    setGlobal(className + "_mt");
    int klass_mt = getTop();

    // set metatable metamethods
    pushCppFunction(&LuaInterface::luaObjectGetEvent);
    setField("__index", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectSetEvent);
    setField("__newindex", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectEqualEvent);
    setField("__eq", klass_mt);
    pushCppFunction(&LuaInterface::luaObjectCollectEvent);
    setField("__gc", klass_mt);

    pushValue(klass);
    setField("methods", klass_mt);
    pushValue(klass_fieldmethods);
    setField("fieldmethods", klass_mt);

    if(!className.empty() && className != "LuaObject") {
        pushValue(klass);
        newTable();
        getGlobal(baseClass);
        setField("__index");
        setMetatable();
        pop();

        pushValue(klass_fieldmethods);
        newTable();
        getGlobal(baseClass + "_fieldmethods");
        setField("__index");
        setMetatable();
        pop();
    }

    pop(3);
}

void LuaInterface::registerClassStaticFunction(const std::string& className,
                                               const std::string& functionName,
                                               const LuaCppFunction& function)
{
    registerClassMemberFunction(className, functionName, function);
}

void LuaInterface::registerClassMemberFunction(const std::string& className,
                                               const std::string& functionName,
                                               const LuaCppFunction& function)
{
    getGlobal(className);
    pushCppFunction(function);
    setField(functionName);
    pop();
}

void LuaInterface::registerClassMemberField(const std::string& className,
                                            const std::string& field,
                                            const LuaCppFunction& getFunction,
                                            const LuaCppFunction& setFunction)
{
    getGlobal(className + "_fieldmethods");

    if(getFunction) {
        pushCppFunction(getFunction);
        setField(stdext::format("get_%s", field));
    }

    if(setFunction) {
        pushCppFunction(setFunction);
        setField(stdext::format("set_%s", field));
    }

    pop();
}

void LuaInterface::registerGlobalFunction(const std::string& functionName, const LuaCppFunction& function)
{
    pushCppFunction(function);
    setGlobal(functionName);
}

int LuaInterface::luaObjectGetEvent(LuaInterface* lua)
{
    LuaObjectPtr obj = lua->toObject(-2);
    std::string key = lua->toString(-1);
    VALIDATE(obj);

    lua->remove(-1);

    lua->getMetatable();
    lua->getField("fieldmethods");
    lua->remove(-2);
    lua->getField(std::string("get_") + key);
    lua->remove(-2);

    if(!lua->isNil()) {
        lua->insert(-2);
        lua->signalCall(1, 1);
        return 1;
    }

    lua->pop();

    obj->luaGetField(key);
    if(!lua->isNil()) {
        lua->remove(-2);
        return 1;
    }

    lua->pop();

    lua->getMetatable();
    lua->getField("methods");
    lua->remove(-2);
    lua->getField(key);
    lua->remove(-2);
    lua->remove(-2);

    return 1;
}

int LuaInterface::luaObjectSetEvent(LuaInterface* lua)
{
    LuaObjectPtr obj = lua->toObject(-3);
    std::string key = lua->toString(-2);
    VALIDATE(obj);

    lua->remove(-2);
    lua->insert(-2);

    lua->getMetatable();
    lua->getField("fieldmethods");
    lua->remove(-2);
    lua->getField(std::string("set_") + key);
    lua->remove(-2);

    if(!lua->isNil()) {
        lua->insert(-3);
        lua->insert(-2);
        lua->signalCall(2, 0);
        return 0;
    }

    lua->pop();
    lua->pop();
    obj->luaSetField(key);
    return 0;
}

int LuaInterface::luaObjectEqualEvent(LuaInterface* lua)
{
    bool ret = false;

    if(lua->isUserdata(-1) && lua->isUserdata(-2)) {
        LuaObjectPtr* objPtr2 = static_cast<LuaObjectPtr*>(lua->popUserdata());
        LuaObjectPtr* objPtr1 = static_cast<LuaObjectPtr*>(lua->popUserdata());

        VALIDATE(objPtr1 && objPtr2);

        if(*objPtr1 == *objPtr2)
            ret = true;
    } else {
        lua->pop(2);
    }

    lua->pushBoolean(ret);
    return 1;
}

int LuaInterface::luaObjectCollectEvent(LuaInterface* lua)
{
    auto objPtr = static_cast<LuaObjectPtr*>(lua->popUserdata());
    VALIDATE(objPtr);

    objPtr->reset();
    g_lua.m_totalObjRefs--;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////

bool LuaInterface::safeRunScript(const std::string& fileName)
{
    try {
        runScript(fileName);
        return true;
    } catch(stdext::exception& e) {
        g_logger.error(stdext::format(
            "Failed to load script '%s': %s",
            fileName,
            e.what()
        ));
        return false;
    }
}

void LuaInterface::runScript(const std::string& fileName)
{
    loadScript(fileName);
    safeCall(0, 0);
}

void LuaInterface::runBuffer(const std::string& buffer, const std::string& source)
{
    loadBuffer(buffer, source);
    safeCall(0, 0);
}

void LuaInterface::loadScript(const std::string& fileName)
{
    std::string filePath = fileName;

    if(!stdext::starts_with(fileName, "/"))
        filePath = getCurrentSourcePath() + "/" + filePath;

    filePath = g_resources.guessFilePath(filePath, "lua");

    std::string buffer = g_resources.readFileContents(filePath);
    std::string source = std::string("@") + filePath;
    loadBuffer(buffer, source);
}

void LuaInterface::loadFunction(const std::string& buffer, const std::string& source)
{
    if(buffer.empty()) {
        pushNil();
        return;
    }

    std::string buf;

    if(stdext::starts_with(buffer, "function"))
        buf = stdext::format("__func = %s", buffer);
    else
        buf = stdext::format("__func = function(self)\n%s\nend", buffer);

    loadBuffer(buf, source);
    safeCall();

    getGlobal("__func");
    pushNil();
    setGlobal("__func");
}

void LuaInterface::evaluateExpression(const std::string& expression,
                                      const std::string& source)
{
    if(!expression.empty()) {
        std::string buffer = stdext::format("__exp = (%s)", expression);
        loadBuffer(buffer, source);
        safeCall();

        getGlobal("__exp");
        pushNil();
        setGlobal("__exp");
    } else {
        pushNil();
    }
}

std::string LuaInterface::traceback(const std::string& errorMessage, int level)
{
    getGlobal("debug");
    getField("traceback");
    remove(-2);

    pushString(errorMessage);
    pushInteger(level);
    call(2, 1);

    return popString();
}

void LuaInterface::throwError(const std::string& message)
{
    if(isInCppCallback()) {
        pushString(message);
        error();
    } else {
        throw stdext::exception(message);
    }
}

std::string LuaInterface::getCurrentSourcePath(int level)
{
    std::string path;

    if(!L)
        return path;

    while(true) {
        getStackFunction(level);

        if(isLuaFunction()) {
            path = functionSourcePath();
            break;
        } else if(isNil()) {
            pop();
            break;
        } else {
            pop();
        }

        level++;
    }

    return path;
}

std::string LuaInterface::getCurrentFunction(int level)
{
    std::string path = "unknown";

    if(!L)
        return path;

    while(true) {
        getStackFunction(level);

        if(isLuaFunction()) {
            path = functionSource();
            break;
        } else if(isNil()) {
            pop();
            break;
        } else {
            pop();
        }

        level++;
    }

    return path;
}

int LuaInterface::safeCall(int numArgs,
                           int numRets,
                           const std::shared_ptr<std::string>& error)
{
    VALIDATE(hasIndex(-numArgs - 1));

    int previousStackSize = stackSize();

    int errorFuncIndex = previousStackSize - numArgs;
    pushCFunction(&LuaInterface::luaErrorHandler);
    insert(errorFuncIndex);

    int ret = pcall(numArgs, LUA_MULTRET, errorFuncIndex);

    remove(errorFuncIndex);

    if(ret != 0) {
        if(error) {
            error->assign(popString());
            clearStack();
            return 0;
        } else {
            std::string errorMsg = popString();
            g_logger.error(stdext::format("Lua error: %s", errorMsg));
            throw LuaException(errorMsg);
        }
    }

    int rets = (stackSize() + numArgs + 1) - previousStackSize;

    while(numRets != -1 && rets != numRets) {
        if(rets < numRets) {
            pushNil();
            rets++;
        } else {
            pop();
            rets--;
        }
    }

    return rets;
}

int LuaInterface::signalCall(int numArgs, int numRets)
{
    int rets = 0;
    int funcIndex = -numArgs - 1;

    try {
        if(isFunction(funcIndex)) {
            static std::shared_ptr<std::string> error =
                std::make_shared<std::string>();

            rets = safeCall(numArgs, -1, error);

            if(!error->empty()) {
                g_logger.error(stdext::format(
                    "protected lua call failed: %s",
                    *error
                ));
                error->clear();
                return rets;
            }

            if(numRets != -1 && rets != numRets)
                throw LuaException(
                    "function call didn't return the expected number of results",
                    0
                );
        }
        else if(isTable(funcIndex)) {
            pushNil();
            bool done = false;

            while(next(funcIndex - 1)) {
                if(isFunction()) {
                    for(int i = 0; i < numArgs; ++i)
                        pushValue(-numArgs - 2);

                    static std::shared_ptr<std::string> error =
                        std::make_shared<std::string>();

                    int callRets = safeCall(numArgs, -1, error);

                    if(!error->empty()) {
                        g_logger.error(stdext::format(
                            "protected lua call failed: %s",
                            *error
                        ));
                        error->clear();
                        return callRets;
                    }

                    if(callRets == 1) {
                        done = popBoolean();

                        if(done) {
                            pop();
                            break;
                        }
                    } else if(callRets != 0) {
                        throw LuaException(
                            "function call didn't return the expected number of results",
                            0
                        );
                    }
                } else {
                    throw LuaException("attempt to call a non function", 0);
                }
            }

            pop(numArgs + 1);

            if(numRets == 1 || numRets == -1) {
                rets = 1;
                pushBoolean(done);
            }
        }
        else if(isNil(funcIndex)) {
            pop(numArgs + 1);
        }
        else {
            throw LuaException("attempt to call a non function value", 0);
        }
    }
    catch(stdext::exception& e) {
        g_logger.error(stdext::format(
            "protected lua call failed: %s",
            e.what()
        ));
    }

    while(numRets != -1 && rets < numRets) {
        pushNil();
        rets++;
    }

    return rets;
}

int LuaInterface::newSandboxEnv()
{
    newTable();
    newTable();

    getRef(getGlobalEnvironment());
    setField("__index");
    setMetatable();

    return ref();
}

///////////////////////////////////////////////////////////////////////////////
// Lua C functions

int LuaInterface::luaScriptLoader(lua_State* L)
{
    std::string fileName = g_lua.popString();

    try {
        g_lua.loadScript(fileName);
        return 1;
    } catch(stdext::exception& e) {
        g_lua.pushString(std::string("\n\t") + e.what());
        return 1;
    }
}

int LuaInterface::lua_dofile(lua_State* L)
{
    std::string file = g_lua.popString();

    try {
        g_lua.loadScript(file);
        g_lua.call(0, LUA_MULTRET);
        return g_lua.stackSize();
    } catch(stdext::exception& e) {
        g_lua.pushString(e.what());
        g_lua.error();
        return 0;
    }
}

int LuaInterface::lua_dofiles(lua_State* L)
{
    std::string contains = "";

    if(g_lua.getTop() > 2)
        contains = g_lua.popString();

    bool recursive = false;

    if(g_lua.getTop() > 1)
        recursive = g_lua.popBoolean();

    std::string directory = g_lua.popString();
    g_lua.loadFiles(directory, recursive, contains);

    return 0;
}

int LuaInterface::lua_loadfile(lua_State* L)
{
    std::string fileName = g_lua.popString();

    try {
        g_lua.loadScript(fileName);
        return 1;
    } catch(stdext::exception& e) {
        g_lua.pushNil();
        g_lua.pushString(e.what());
        g_lua.error();
        return 2;
    }
}

int LuaInterface::luaErrorHandler(lua_State* L)
{
    auto error = g_lua.popString();

    if(error.find("stack traceback:") == std::string::npos)
        error = g_lua.traceback(error, 1);

    g_lua.pushString(error);
    return 1;
}

int LuaInterface::luaCppFunctionCallback(lua_State* L)
{
    auto funcPtr =
        static_cast<LuaCppFunctionPtr*>(g_lua.popUpvalueUserdata());

    VALIDATE(funcPtr);

    int numRets = 0;

    try {
        g_lua.m_cppCallbackDepth++;

        numRets = (*(funcPtr->get()))(&g_lua);

        g_lua.m_cppCallbackDepth--;

#ifndef NDEBUG
        if(numRets != g_lua.stackSize()) {
            throw stdext::exception(stdext::format(
                "LuaInterface::luaCppFunctionCallback, "
                "numRets != g_lua.stackSize() (%i != %i)",
                numRets,
                g_lua.stackSize()
            ));
        }
#endif
    }
    catch(stdext::exception& e) {
        while(g_lua.stackSize() > 0)
            g_lua.pop();

        numRets = 0;

        g_lua.pushString(stdext::format(
            "C++ call failed: %s",
            g_lua.traceback(e.what())
        ));

        g_lua.error();
    }
    catch(const std::exception& e) {
        g_logger.fatal(stdext::format(
            "Critical lua error!\n"
            "C++ exception: %s\n"
            "Function: %s\n%s",
            e.what(),
            g_lua.getCurrentFunction(),
            g_lua.traceback(e.what())
        ));
    }
    catch(...) {
        g_logger.fatal(stdext::format(
            "Critical lua error!\n"
            "Unknown C++ exception\n"
            "Function: %s\n%s",
            g_lua.getCurrentFunction(),
            g_lua.traceback("unknown C++ exception")
        ));
    }

    return numRets;
}

int LuaInterface::luaCollectCppFunction(lua_State* L)
{
    auto funcPtr =
        static_cast<LuaCppFunctionPtr*>(g_lua.popUserdata());

    VALIDATE(funcPtr);

    funcPtr->reset();
    g_lua.m_totalFuncRefs--;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Lua API

void LuaInterface::createLuaState()
{
    L = luaL_newstate();

    if(!L)
        g_logger.fatal("Unable to create lua state");

    luaL_openlibs(L);

    luaopen_bit32(L);

    newTable();
    newTable();

    pushString("v");
    setField("__mode");
    setMetatable();

    m_weakTableRef = ref();

    getGlobal("package");
    getField("loaders");

    pushCFunction(&LuaInterface::luaScriptLoader);
    rawSeti(5);
    pop(2);

    pushCFunction(&LuaInterface::lua_dofile);
    setGlobal("dofile");

    pushCFunction(&LuaInterface::lua_dofiles);
    setGlobal("dofiles");

    pushCFunction(&LuaInterface::lua_loadfile);
    setGlobal("loadfile");

#ifdef FREE_VERSION
    pushCFunction(nullptr);
    setGlobal("io");
#endif
}

void LuaInterface::closeLuaState()
{
    if(L) {
        lua_close(L);
        L = NULL;
    }
}

void LuaInterface::collectGarbage()
{
    static bool collecting = false;

    if(!collecting) {
        collecting = true;

        for(int i = 0; i < 2; ++i)
            lua_gc(L, LUA_GCCOLLECT, 0);

        collecting = false;
    }
}

void LuaInterface::loadBuffer(const std::string& buffer,
                              const std::string& source)
{
#ifdef FREE_VERSION
    for(auto& c : buffer) {
        if((uint8_t)c < 8) {
            throw LuaException(
                "Compiled lua files are not allowed in free version",
                0
            );
        }
    }
#endif

    int ret = luaL_loadbuffer(
        L,
        buffer.c_str(),
        buffer.length(),
        source.c_str()
    );

    if(ret != 0)
        throw LuaException(popString(), 0);
}

int dumpWriter(lua_State* L, const void* p, size_t sz, void* ud)
{
    if(!ud)
        return -1;

    std::string* ret = (std::string*)ud;
    ret->append((const char*)p, sz);

    return 0;
}

std::string LuaInterface::generateByteCode(const std::string& buffer,
                                           std::string source)
{
    stdext::replace_all(source, "\\", "/");
    source = std::string("@/") + source;

    std::string ret;

    int status = luaL_loadbuffer(
        L,
        buffer.c_str(),
        buffer.length(),
        source.c_str()
    );

    if(status != 0)
        return ret;

    lua_dump(L, dumpWriter, &ret);
    clearStack();

    return ret;
}

int LuaInterface::pcall(int numArgs,
                        int numRets,
                        int errorFuncIndex)
{
    VALIDATE(hasIndex(-numArgs - 1));
    return lua_pcall(L, numArgs, numRets, errorFuncIndex);
}

void LuaInterface::call(int numArgs, int numRets)
{
    VALIDATE(hasIndex(-numArgs - 1));
    lua_call(L, numArgs, numRets);
}

void LuaInterface::error()
{
    VALIDATE(hasIndex(-1));
    lua_error(L);
}

int LuaInterface::ref()
{
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    VALIDATE(ref != LUA_NOREF);
    VALIDATE(ref < 2147483647);

    return ref;
}

int LuaInterface::weakRef()
{
    static int id = 0;

    ++id;

    if(id == 2147483647)
        id = 0;

    getRef(m_weakTableRef);
    insert(-2);
    rawSeti(id);
    pop();

    return id;
}

void LuaInterface::unref(int ref)
{
    if(ref >= 0 && L != NULL)
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

const char* LuaInterface::typeName(int index)
{
    VALIDATE(hasIndex(index));

    int type = lua_type(L, index);
    return lua_typename(L, type);
}

std::string LuaInterface::functionSourcePath()
{
    std::string path;

    lua_Debug ar;
    memset(&ar, 0, sizeof(ar));
    lua_getinfo(L, ">Sn", &ar);

    if(ar.source) {
        if(ar.source[0] == '@') {
            path = ar.source;
            path = path.substr(1, path.find_last_of("/") - 1);
            path = path.substr(0, path.find_last_of(":"));
        }
    }

    return path;
}

std::string LuaInterface::functionSource()
{
    std::string path;

    lua_Debug ar;
    memset(&ar, 0, sizeof(ar));
    lua_getinfo(L, ">Sn", &ar);

    if(ar.source)
        path = ar.source;

    return path;
}

void LuaInterface::insert(int index)
{
    VALIDATE(hasIndex(index));
    lua_insert(L, index);
}

void LuaInterface::remove(int index)
{
    VALIDATE(hasIndex(index));
    lua_remove(L, index);
}

bool LuaInterface::next(int index)
{
    VALIDATE(hasIndex(index));
    return lua_next(L, index);
}

void LuaInterface::getStackFunction(int level)
{
    lua_Debug ar;

    if(lua_getstack(L, level, &ar) == 1)
        lua_getinfo(L, "f", &ar);
    else
        pushNil();
}

void LuaInterface::getRef(int ref)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
}

void LuaInterface::getWeakRef(int weakRef)
{
    getRef(m_weakTableRef);
    rawGeti(weakRef);
    remove(-2);
}

void LuaInterface::setGlobalEnvironment(int env)
{
    pushThread();
    getRef(env);

    VALIDATE(isTable());

    setEnv();
    pop();
}

void LuaInterface::setMetatable(int index)
{
    VALIDATE(hasIndex(index));
    lua_setmetatable(L, index);
}

void LuaInterface::getMetatable(int index)
{
    VALIDATE(hasIndex(index));
    lua_getmetatable(L, index);
}

void LuaInterface::getField(const char* key, int index)
{
    VALIDATE(hasIndex(index));
    VALIDATE(isUserdata(index) || isTable(index));

    lua_getfield(L, index, key);
}

void LuaInterface::setField(const char* key, int index)
{
    VALIDATE(hasIndex(index));
    VALIDATE(isUserdata(index) || isTable(index));

    lua_setfield(L, index, key);
}

void LuaInterface::getEnv(int index)
{
    VALIDATE(hasIndex(index));
    lua_getfenv(L, index);
}

void LuaInterface::setEnv(int index)
{
    VALIDATE(hasIndex(index));
    lua_setfenv(L, index);
}

void LuaInterface::getTable(int index)
{
    VALIDATE(hasIndex(index));
    lua_gettable(L, index);
}

void LuaInterface::setTable(int index)
{
    VALIDATE(hasIndex(index));
    lua_settable(L, index);
}

void LuaInterface::clearTable(int index)
{
    VALIDATE(hasIndex(index) && isTable(index));

    pushNil();

    while(next(index - 1)) {
        pop();
        pushValue();

        if(next(index - 2)) {
            pop();
            insert(-2);
            pushNil();
            rawSet(index - 3);
        } else {
            pushNil();
            rawSet(index - 2);
            break;
        }
    }
}

void LuaInterface::getGlobal(const std::string& key)
{
    lua_getglobal(L, key.c_str());
}

void LuaInterface::getGlobalField(const std::string& globalKey,
                                  const std::string& fieldKey)
{
    getGlobal(globalKey);

    if(!isNil()) {
        VALIDATE(isTable() || isUserdata());
        getField(fieldKey);
        remove(-2);
    }
}

void LuaInterface::setGlobal(const std::string& key)
{
    VALIDATE(hasIndex(-1));
    lua_setglobal(L, key.c_str());
}

void LuaInterface::rawGet(int index)
{
    VALIDATE(hasIndex(index));
    lua_rawget(L, index);
}

void LuaInterface::rawGeti(int n, int index)
{
    VALIDATE(hasIndex(index));
    lua_rawgeti(L, index, n);
}

void LuaInterface::rawSet(int index)
{
    VALIDATE(hasIndex(index));
    lua_rawset(L, index);
}

void LuaInterface::rawSeti(int n, int index)
{
    VALIDATE(hasIndex(index));
    lua_rawseti(L, index, n);
}

void LuaInterface::newTable()
{
    lua_newtable(L);
}

void LuaInterface::createTable(int narr, int nrec)
{
    lua_createtable(L, narr, nrec);
}

void* LuaInterface::newUserdata(int size)
{
    return lua_newuserdata(L, size);
}

void LuaInterface::pop(int n)
{
    if(n > 0) {
        VALIDATE(hasIndex(-n));
        lua_pop(L, n);
    }
}

long LuaInterface::popInteger()
{
    VALIDATE(hasIndex(-1));
    long v = toInteger(-1);
    pop();
    return v;
}

double LuaInterface::popNumber()
{
    VALIDATE(hasIndex(-1));
    double v = toNumber(-1);
    pop();
    return v;
}

bool LuaInterface::popBoolean()
{
    VALIDATE(hasIndex(-1));
    bool v = toBoolean(-1);
    pop();
    return v;
}

std::string LuaInterface::popString()
{
    VALIDATE(hasIndex(-1));
    std::string v = toString(-1);
    pop();
    return v;
}

void* LuaInterface::popUserdata()
{
    VALIDATE(hasIndex(-1));
    void* v = toUserdata(-1);
    pop();
    return v;
}

LuaObjectPtr LuaInterface::popObject()
{
    VALIDATE(hasIndex(-1));
    LuaObjectPtr v = toObject(-1);
    pop();
    return v;
}

void* LuaInterface::popUpvalueUserdata()
{
    return lua_touserdata(L, lua_upvalueindex(1));
}

void LuaInterface::pushNil()
{
    lua_pushnil(L);
    checkStack();
}

void LuaInterface::pushInteger(long v)
{
    lua_pushinteger(L, v);
    checkStack();
}

void LuaInterface::pushNumber(double v)
{
    lua_pushnumber(L, v);
    checkStack();
}

void LuaInterface::pushBoolean(bool v)
{
    lua_pushboolean(L, v);
    checkStack();
}

void LuaInterface::pushCString(const char* v)
{
    if(!v)
        lua_pushnil(L);
    else
        lua_pushstring(L, v);

    checkStack();
}

void LuaInterface::pushString(const std::string& v)
{
    lua_pushlstring(L, v.c_str(), v.length());
    checkStack();
}

void LuaInterface::pushLightUserdata(void* p)
{
    lua_pushlightuserdata(L, p);
    checkStack();
}

void LuaInterface::pushThread()
{
    lua_pushthread(L);
    checkStack();
}

void LuaInterface::pushObject(const LuaObjectPtr& obj)
{
    new(newUserdata(sizeof(LuaObjectPtr))) LuaObjectPtr(obj);
    m_totalObjRefs++;

    obj->luaGetMetatable();

    if(isNil()) {
        g_logger.fatal(stdext::format(
            "metatable for class '%s' not found, did you bind the C++ class?",
            obj->getClassName()
        ));
    }

    setMetatable();
}

void LuaInterface::pushCFunction(LuaCFunction func, int n)
{
    lua_pushcclosure(L, func, n);
    checkStack();
}

void LuaInterface::pushCppFunction(const LuaCppFunction& func)
{
    new(newUserdata(sizeof(LuaCppFunctionPtr)))
        LuaCppFunctionPtr(new LuaCppFunction(func));

    m_totalFuncRefs++;

    newTable();
    pushCFunction(&LuaInterface::luaCollectCppFunction);
    setField("__gc");
    setMetatable();

    pushCFunction(&LuaInterface::luaCppFunctionCallback, 1);
}

void LuaInterface::pushValue(int index)
{
    VALIDATE(hasIndex(index));
    lua_pushvalue(L, index);
    checkStack();
}

bool LuaInterface::isNil(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isnil(L, index);
}

bool LuaInterface::isBoolean(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isboolean(L, index);
}

bool LuaInterface::isNumber(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isnumber(L, index);
}

bool LuaInterface::isString(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isstring(L, index);
}

bool LuaInterface::isTable(int index)
{
    VALIDATE(hasIndex(index));
    return lua_istable(L, index);
}

bool LuaInterface::isFunction(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isfunction(L, index);
}

bool LuaInterface::isCFunction(int index)
{
    VALIDATE(hasIndex(index));
    return lua_iscfunction(L, index);
}

bool LuaInterface::isUserdata(int index)
{
    VALIDATE(hasIndex(index));
    return lua_isuserdata(L, index);
}

bool LuaInterface::toBoolean(int index)
{
    VALIDATE(hasIndex(index));
    return (bool)lua_toboolean(L, index);
}

int LuaInterface::toInteger(int index)
{
    VALIDATE(hasIndex(index));
    return lua_tointeger(L, index);
}

double LuaInterface::toNumber(int index)
{
    VALIDATE(hasIndex(index));
    return lua_tonumber(L, index);
}

const char* LuaInterface::toCString(int index)
{
    VALIDATE(hasIndex(index));
    return lua_tostring(L, index);
}

std::string LuaInterface::toString(int index)
{
    VALIDATE(hasIndex(index));

    std::string str;
    size_t len;

    const char* c_str = lua_tolstring(L, index, &len);

    if(c_str && len > 0)
        str.assign(c_str, len);

    return str;
}

void* LuaInterface::toUserdata(int index)
{
    VALIDATE(hasIndex(index));
    return lua_touserdata(L, index);
}

LuaObjectPtr LuaInterface::toObject(int index)
{
    VALIDATE(hasIndex(index));

    if(isUserdata(index)) {
        LuaObjectPtr* objRef =
            static_cast<LuaObjectPtr*>(toUserdata(index));

        if(objRef && *objRef)
            return *objRef;
    }

    return nullptr;
}

int LuaInterface::getTop()
{
    return lua_gettop(L);
}

std::string LuaInterface::getSource(int level)
{
    lua_Debug ar;

    ar.short_src[0] = 0;
    ar.currentline = 0;

    if(lua_getstack(L, level, &ar) == 1)
        lua_getinfo(L, "Sl", &ar);

    return std::string(ar.short_src) + ":" +
           std::to_string(ar.currentline);
}

void LuaInterface::loadFiles(std::string directory,
                             bool recursive,
                             std::string contains)
{
    for(const std::string& fileName :
        g_resources.listDirectoryFiles(directory)) {

        std::string fullPath = directory + "/" + fileName;

        if(recursive && g_resources.directoryExists(fullPath)) {
            loadFiles(fullPath, true, contains);
            continue;
        }

        if(!g_resources.isFileType(fileName, "lua"))
            continue;

        if(!contains.empty() &&
           fileName.find(contains) == std::string::npos)
            continue;

        try {
            g_lua.loadScript(fullPath);
            g_lua.call(0, 0);
        } catch(stdext::exception& e) {
            g_lua.pushString(e.what());
            g_lua.error();
        }
    }
}
