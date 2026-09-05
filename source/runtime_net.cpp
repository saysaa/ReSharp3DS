#include "runtime_net.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define DOWNLOAD_BUFFER_SIZE (32 * 1024)
#define DOWNLOAD_TMP_SUFFIX ".tmp"
#define MAX_REDIRECTS 8
#define MAX_URL_SIZE 1024

bool FileExists(const char* path)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

bool DirectoryExists(const char* path)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureDirectory(const char* path)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    if (DirectoryExists(path))
    {
        return true;
    }

    int result = mkdir(path, 0777);
    return result == 0 || errno == EEXIST;
}

static bool MakeTemporaryPath(const char* outputPath, char* tmpPath, size_t tmpPathSize)
{
    if (outputPath == NULL || outputPath[0] == '\0' || tmpPath == NULL || tmpPathSize == 0)
    {
        return false;
    }

    int written = snprintf(tmpPath, tmpPathSize, "%s%s", outputPath, DOWNLOAD_TMP_SUFFIX);
    return written > 0 && written < (int)tmpPathSize;
}

static void DeleteFileIfExists(const char* path)
{
    if (path != NULL && path[0] != '\0')
    {
        remove(path);
    }
}

static bool StartsWith(const char* text, const char* prefix)
{
    if (!text || !prefix)
    {
        return false;
    }

    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool ResolveRedirectUrl(const char* currentUrl, const char* location, char* output, size_t outputSize)
{
    if (!currentUrl || !location || !output || outputSize == 0)
    {
        return false;
    }

    output[0] = '\0';

    if (StartsWith(location, "http://") || StartsWith(location, "https://"))
    {
        int written = snprintf(output, outputSize, "%s", location);
        return written > 0 && written < (int)outputSize;
    }

    if (location[0] == '/')
    {
        const char* schemeEnd = strstr(currentUrl, "://");
        if (!schemeEnd) return false;

        const char* hostStart = schemeEnd + 3;
        const char* hostEnd = strchr(hostStart, '/');
        int baseLen = hostEnd ? (int)(hostEnd - currentUrl) : (int)strlen(currentUrl);

        int written = snprintf(output, outputSize, "%.*s%s", baseLen, currentUrl, location);
        return written > 0 && written < (int)outputSize;
    }

    char base[MAX_URL_SIZE];
    snprintf(base, sizeof(base), "%s", currentUrl);

    char* slash = strrchr(base, '/');
    if (!slash) return false;

    slash[1] = '\0';

    int written = snprintf(output, outputSize, "%s%s", base, location);
    return written > 0 && written < (int)outputSize;
}

static void CloseHttp(httpcContext* context)
{
    if (context != NULL)
    {
        httpcCloseContext(context);
    }

    httpcExit();
}

static bool OpenHttpGet(const char* url, httpcContext* context, u32* statusCode)
{
    if (!url || !context || !statusCode)
    {
        return false;
    }

    *statusCode = 0;
    memset(context, 0, sizeof(httpcContext));

    Result rc = httpcInit(0);
    if (R_FAILED(rc))
    {
        printf("[HTTP] httpcInit failed: 0x%08lX\n", (unsigned long)rc);
        return false;
    }

    printf("[HTTP] open: %s\n", url);

    rc = httpcOpenContext(context, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(rc))
    {
        printf("[HTTP] httpcOpenContext failed: 0x%08lX\n", (unsigned long)rc);
        httpcExit();
        return false;
    }

    httpcSetSSLOpt(context, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(context, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(context, "User-Agent", "ReSharp3DS");
    httpcAddRequestHeaderField(context, "Accept", "*/*");
    httpcAddRequestHeaderField(context, "Connection", "close");

    rc = httpcBeginRequest(context);
    if (R_FAILED(rc))
    {
        printf("[HTTP] httpcBeginRequest failed: 0x%08lX\n", (unsigned long)rc);
        CloseHttp(context);
        return false;
    }

    rc = httpcGetResponseStatusCode(context, statusCode);
    if (R_FAILED(rc))
    {
        printf("[HTTP] status failed: 0x%08lX\n", (unsigned long)rc);
        CloseHttp(context);
        return false;
    }

    printf("[HTTP] status=%lu\n", (unsigned long)*statusCode);
    return true;
}

static bool DrainHttpBody(httpcContext* context)
{
    if (!context)
    {
        return false;
    }

    static u8 buffer[DOWNLOAD_BUFFER_SIZE];

    while (true)
    {
        u32 downloaded = 0;
        Result rc = httpcDownloadData(context, buffer, DOWNLOAD_BUFFER_SIZE, &downloaded);

        if (rc == 0)
        {
            return true;
        }

        if (rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
        {
            return false;
        }

        gspWaitForVBlank();
    }
}

static bool DownloadFileInternal(const char* url, const char* outputPath, int redirectDepth)
{
    if (url == NULL || url[0] == '\0' || outputPath == NULL || outputPath[0] == '\0')
    {
        return false;
    }

    if (redirectDepth > MAX_REDIRECTS)
    {
        printf("[HTTP] too many redirects\n");
        return false;
    }

    char tmpPath[512];
    if (!MakeTemporaryPath(outputPath, tmpPath, sizeof(tmpPath)))
    {
        printf("[HTTP] temporary path too long\n");
        return false;
    }

    DeleteFileIfExists(tmpPath);

    httpcContext context;
    u32 statusCode = 0;

    if (!OpenHttpGet(url, &context, &statusCode))
    {
        return false;
    }

    if (statusCode >= 300 && statusCode < 400)
    {
        char location[512];
        char resolved[MAX_URL_SIZE];
        location[0] = '\0';
        resolved[0] = '\0';

        Result headerRc = httpcGetResponseHeader(&context, "Location", location, sizeof(location));
        DrainHttpBody(&context);
        CloseHttp(&context);

        if (R_FAILED(headerRc) || location[0] == '\0')
        {
            printf("[HTTP] redirect without Location\n");
            return false;
        }

        if (!ResolveRedirectUrl(url, location, resolved, sizeof(resolved)))
        {
            printf("[HTTP] cannot resolve redirect URL\n");
            return false;
        }

        printf("[HTTP] redirect -> %s\n", resolved);
        return DownloadFileInternal(resolved, outputPath, redirectDepth + 1);
    }

    if (statusCode != 200)
    {
        printf("[HTTP] bad status code\n");
        DrainHttpBody(&context);
        CloseHttp(&context);
        return false;
    }

    FILE* file = fopen(tmpPath, "wb");
    if (!file)
    {
        printf("[HTTP] fopen failed: %s\n", tmpPath);
        DrainHttpBody(&context);
        CloseHttp(&context);
        return false;
    }

    static u8 buffer[DOWNLOAD_BUFFER_SIZE];

    while (true)
    {
        u32 downloaded = 0;
        Result rc = httpcDownloadData(&context, buffer, DOWNLOAD_BUFFER_SIZE, &downloaded);

        if (downloaded > 0)
        {
            size_t written = fwrite(buffer, 1, downloaded, file);
            if (written != downloaded)
            {
                fclose(file);
                DeleteFileIfExists(tmpPath);
                CloseHttp(&context);
                return false;
            }
        }

        if (rc == 0)
        {
            break;
        }

        if (rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
        {
            printf("[HTTP] download failed: 0x%08lX\n", (unsigned long)rc);
            fclose(file);
            DeleteFileIfExists(tmpPath);
            CloseHttp(&context);
            return false;
        }

        gspWaitForVBlank();
    }

    fclose(file);
    CloseHttp(&context);

    DeleteFileIfExists(outputPath);

    if (rename(tmpPath, outputPath) != 0)
    {
        printf("[HTTP] rename failed errno=%d\n", errno);
        DeleteFileIfExists(tmpPath);
        return false;
    }

    printf("[HTTP] downloaded OK: %s\n", outputPath);
    return FileExists(outputPath);
}

bool DownloadFile(const char* url, const char* outputPath)
{
    return DownloadFileInternal(url, outputPath, 0);
}
