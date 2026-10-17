//
// log.cpp - debug logging for BlueJ2534
//
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "log.h"

static FILE* g_logFile = NULL;
static bool g_enabled = false;
static CRITICAL_SECTION g_logLock;
static bool g_lockInit = false;

void LogInit(const char* dllDir, bool enabled)
{
    if (!g_lockInit) {
        InitializeCriticalSection(&g_logLock);
        g_lockInit = true;
    }
    g_enabled = enabled;
    if (!enabled || g_logFile)
        return;

    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\BlueJ2534.log", dllDir);
    path[MAX_PATH - 1] = 0;
    g_logFile = fopen(path, "a");
    if (!g_logFile) {
        // Install dir may be unwritable (Program Files, non-elevated app):
        // fall back to %LOCALAPPDATA%\BlueJ2534\BlueJ2534.log.
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            char dir[MAX_PATH];
            _snprintf(dir, sizeof(dir), "%s\\BlueJ2534", base);
            dir[MAX_PATH - 1] = 0;
            CreateDirectoryA(dir, NULL);
            _snprintf(path, sizeof(path), "%s\\BlueJ2534.log", dir);
            path[MAX_PATH - 1] = 0;
            g_logFile = fopen(path, "a");
        }
    }
    if (g_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logFile, "\n===== BlueJ2534 session %04d-%02d-%02d %02d:%02d:%02d =====\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_logFile);
    }
}

void LogShutdown()
{
    if (!g_lockInit) {
        if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
        return;
    }
    EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
    LeaveCriticalSection(&g_logLock);
}

bool LogEnabled()
{
    return g_enabled && g_logFile != NULL;
}

void Log(const char* fmt, ...)
{
    if (!g_enabled || !g_logFile || !g_lockInit)
        return;

    EnterCriticalSection(&g_logLock);
    if (!g_logFile) {               // closed while we were outside the lock
        LeaveCriticalSection(&g_logLock);
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    LeaveCriticalSection(&g_logLock);
}
