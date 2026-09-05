#ifndef RUNTIME_NET_H
#define RUNTIME_NET_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool FileExists(const char* path);
bool DirectoryExists(const char* path);
bool EnsureDirectory(const char* path);
bool DownloadFile(const char* url, const char* outputPath);

#ifdef __cplusplus
}
#endif

#endif
