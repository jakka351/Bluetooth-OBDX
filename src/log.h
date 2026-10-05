//
// log.h - debug logging for BlueJ2534
//
#pragma once

void LogInit(const char* dllDir, bool enabled);
void LogShutdown();
void Log(const char* fmt, ...);
bool LogEnabled();
