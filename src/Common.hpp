#pragma once

#include <string>
#include <cstdint>

// Directory of the running executable (UTF-8, with trailing backslash).
extern std::string  g_exeDir;
extern std::wstring g_exeDirWide;

std::string  Utf8FromWide(const std::wstring& w);
std::wstring WideFromUtf8(const std::string& s);
bool         ReadAllBytes(const std::string& pathUtf8, std::string& out);
std::string  PathJoin(const std::string& a, const std::string& b);
void         LogMsg(const char* fmt, ...);
