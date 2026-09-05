#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include "gui.h"

#include <nanoCLR_Types.h>
#include <nanoCLR_Runtime.h>
#include <nanoCLR_Application.h>
#include <nanoCLR_Interop.h>


#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif

extern "C" {
    HRESULT nanoCLR_Initialize(const void* settings);
    void nanoCLR_Cleanup();
}

static CLR_RT_Assembly* g_mscorlibAssembly = NULL;
static CLR_RT_Assembly* g_appAssembly = NULL;

static CLR_RT_MethodHandler g_appNativeMethods[1024];
static CLR_RT_MethodHandler g_mscorlibNativeMethods[1024];

static bool g_graphicsUsed = false;
static bool g_consoleModeScreenShown = false;
static char g_selectedAppPath[256] = { 0 };
static char g_selectedAppDir[256] = { 0 };
static char g_selectedAppName[128] = { 0 };
static u32 g_randomSeed = 0;

static const char* RUNTIME_VERSION = "v2.0.3-beta.9";
static bool g_runtimeExitRequested = false;
static bool g_runtimeRestartRequested = false;
static int g_runtimeFps = 0;
static u32 g_runtimeFrameCounter = 0;
static u64 g_runtimeFpsLastTime = 0;
static bool g_musicLoopEnabled = true;
static bool g_musicPaused = false;

static u32 g_inputKeysHeld = 0;
static u32 g_inputKeysDown = 0;
static touchPosition g_inputTouch;
static circlePosition g_inputCircle;
static gfxScreen_t g_graphicsTarget = GFX_BOTTOM;


// ------------------------------------------------------------
// App path helpers
// ------------------------------------------------------------

static void SetSelectedAppDirectory(const char* appPath)
{
    g_selectedAppPath[0] = '\0';
    g_selectedAppDir[0] = '\0';
    g_selectedAppName[0] = '\0';

    if (appPath == NULL || appPath[0] == '\0')
    {
        return;
    }

    snprintf(g_selectedAppPath, sizeof(g_selectedAppPath), "%s", appPath);
    snprintf(g_selectedAppDir, sizeof(g_selectedAppDir), "%s", appPath);

    int lastSlash = -1;

    for (int i = 0; g_selectedAppDir[i] != '\0'; i++)
    {
        if (g_selectedAppDir[i] == '/' || g_selectedAppDir[i] == '\\')
        {
            lastSlash = i;
        }
    }

    if (lastSlash >= 0)
    {
        snprintf(g_selectedAppName, sizeof(g_selectedAppName), "%s", g_selectedAppDir + lastSlash + 1);
        g_selectedAppDir[lastSlash + 1] = '\0';
    }
    else
    {
        snprintf(g_selectedAppName, sizeof(g_selectedAppName), "%s", g_selectedAppDir);
        g_selectedAppDir[0] = '\0';
    }

    printf("[APP] path=%s\n", g_selectedAppPath);
    printf("[APP] dir=%s\n", g_selectedAppDir);
    printf("[APP] name=%s\n", g_selectedAppName);
}

static bool IsAbsolutePath(const char* path)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    if (path[0] == '/')
    {
        return true;
    }

    // Handles "sdmc:/...", "romfs:/...", etc.
    for (int i = 0; path[i] != '\0' && i < 16; i++)
    {
        if (path[i] == ':' && path[i + 1] == '/')
        {
            return true;
        }

        if (path[i] == '/' || path[i] == '\\')
        {
            break;
        }
    }

    return false;
}

static void ResolveAppRelativePath(const char* inputPath, char* outputPath, size_t outputSize)
{
    if (outputPath == NULL || outputSize == 0)
    {
        return;
    }

    outputPath[0] = '\0';

    if (inputPath == NULL || inputPath[0] == '\0')
    {
        return;
    }

    if (IsAbsolutePath(inputPath))
    {
        snprintf(outputPath, outputSize, "%s", inputPath);
        return;
    }

    snprintf(outputPath, outputSize, "%s%s", g_selectedAppDir, inputPath);
}


static int GetGraphicsWidth()
{
    return g_graphicsTarget == GFX_TOP ? 400 : 320;
}

static int GetGraphicsHeight()
{
    return 240;
}

static void UpdateRuntimeInputSnapshot()
{
    hidScanInput();
    g_inputKeysHeld = hidKeysHeld();
    g_inputKeysDown = hidKeysDown();
    hidTouchRead(&g_inputTouch);
    hidCircleRead(&g_inputCircle);
}

static void EnsureLogDirectory()
{
    mkdir("sdmc:/ReSharp3DS", 0777);
    mkdir("sdmc:/ReSharp3DS/logs", 0777);
}

static void AppendRuntimeLog(const char* level, const char* text)
{
    if (level == NULL) level = "INFO";
    if (text == NULL) text = "";

    EnsureLogDirectory();

    char logPath[512];
    const char* appName = g_selectedAppName[0] != '\0' ? g_selectedAppName : "runtime";
    snprintf(logPath, sizeof(logPath), "sdmc:/ReSharp3DS/logs/%s.log", appName);

    FILE* file = fopen(logPath, "ab");
    if (!file)
    {
        printf("[LOG] open failed: %s\n", logPath);
        return;
    }

    fprintf(file, "[%llu] [%s] %s\n", (unsigned long long)osGetTime(), level, text);
    fclose(file);
}

static void SaveCrashLog(const char* reason, HRESULT hr)
{
    EnsureLogDirectory();

    FILE* file = fopen("sdmc:/ReSharp3DS/logs/crash.txt", "ab");
    if (!file)
    {
        return;
    }

    fprintf(file, "=== ReSharp3DS crash ===\n");
    fprintf(file, "time=%llu\n", (unsigned long long)osGetTime());
    fprintf(file, "app=%s\n", g_selectedAppPath[0] ? g_selectedAppPath : "unknown");
    fprintf(file, "reason=%s\n", reason ? reason : "unknown");
    fprintf(file, "hr=0x%08X\n\n", (unsigned)hr);
    fclose(file);
}

static void ShowCrashScreen(const char* reason, HRESULT hr)
{
    consoleClear();

    printf("\x1b[31mReSharp3DS crashed\x1b[0m\n\n");
    printf("Reason: %s\n", reason ? reason : "unknown");
    printf("HRESULT: 0x%08X\n\n", (unsigned)hr);
    printf("Crash log saved to:\n");
    printf("sdmc:/ReSharp3DS/logs/crash.txt\n\n");
    printf("Press START to exit\n");
}


// ------------------------------------------------------------
// mscorlib temporary patch
// ------------------------------------------------------------

static HRESULT Native_Nop(CLR_RT_StackFrame& stack)
{
    return S_OK;
}

static void InitMscorlibNativeTable()
{
    for (int i = 0; i < 1024; i++)
    {
        g_mscorlibNativeMethods[i] = NULL;
    }

    g_mscorlibNativeMethods[3] = Native_Nop;
    g_mscorlibAssembly->m_nativeCode = g_mscorlibNativeMethods;

    printf("[PATCH] mscorlib method=3 patched as NOP\n");
}

// ------------------------------------------------------------
// Console API
// ------------------------------------------------------------

static HRESULT Native_Clear(CLR_RT_StackFrame& stack)
{
    consoleClear();
    return S_OK;
}

static HRESULT Native_Write(CLR_RT_StackFrame& stack)
{
    CLR_RT_HeapBlock& arg = stack.Arg0();
    const char* text = arg.RecoverString();

    if (text == NULL)
    {
        text = "";
    }

    printf("%s", text);
    return S_OK;
}

static HRESULT Native_WriteLine(CLR_RT_StackFrame& stack)
{
    CLR_RT_HeapBlock& arg = stack.Arg0();
    const char* text = arg.RecoverString();

    if (text == NULL)
    {
        text = "";
    }

    printf("%s\n", text);
    return S_OK;
}

static HRESULT Native_WriteInt(CLR_RT_StackFrame& stack)
{
    int value = stack.Arg0().NumericByRef().s4;
    printf("%d", value);
    return S_OK;
}

static HRESULT Native_WriteLineInt(CLR_RT_StackFrame& stack)
{
    int value = stack.Arg0().NumericByRef().s4;
    printf("%d\n", value);
    return S_OK;
}

// ------------------------------------------------------------
// Input API
// ------------------------------------------------------------

static HRESULT Native_IsKeyPressed(CLR_RT_StackFrame& stack, u32 key)
{
    stack.SetResult_I4((g_inputKeysHeld & key) ? 1 : 0);

    return S_OK;
}

static HRESULT Native_IsKeyPressedOnce(CLR_RT_StackFrame& stack, u32 key)
{
    stack.SetResult_I4((g_inputKeysDown & key) ? 1 : 0);

    return S_OK;
}

static HRESULT Native_IsStartPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_START);
}

static HRESULT Native_IsSelectPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_SELECT);
}

static HRESULT Native_IsAPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_A);
}

static HRESULT Native_IsBPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_B);
}

static HRESULT Native_IsXPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_X);
}

static HRESULT Native_IsYPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_Y);
}

static HRESULT Native_IsLPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_L);
}

static HRESULT Native_IsRPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_R);
}

static HRESULT Native_IsUpPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_DUP);
}

static HRESULT Native_IsDownPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_DDOWN);
}

static HRESULT Native_IsLeftPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_DLEFT);
}

static HRESULT Native_IsRightPressedOnce(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressedOnce(stack, KEY_DRIGHT);
}

static HRESULT Native_IsStartPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_START);
}

static HRESULT Native_IsSelectPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_SELECT);
}

static HRESULT Native_IsAPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_A);
}

static HRESULT Native_IsBPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_B);
}

static HRESULT Native_IsXPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_X);
}

static HRESULT Native_IsYPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_Y);
}

static HRESULT Native_IsLPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_L);
}

static HRESULT Native_IsRPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_R);
}

static HRESULT Native_IsUpPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_DUP);
}

static HRESULT Native_IsDownPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_DDOWN);
}

static HRESULT Native_IsLeftPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_DLEFT);
}

static HRESULT Native_IsRightPressed(CLR_RT_StackFrame& stack)
{
    return Native_IsKeyPressed(stack, KEY_DRIGHT);
}

static HRESULT Native_TouchIsPressed(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((g_inputKeysHeld & KEY_TOUCH) ? 1 : 0);
    return S_OK;
}

static HRESULT Native_TouchX(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)g_inputTouch.px);
    return S_OK;
}

static HRESULT Native_TouchY(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)g_inputTouch.py);
    return S_OK;
}

static HRESULT Native_CirclePadX(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)g_inputCircle.dx);
    return S_OK;
}

static HRESULT Native_CirclePadY(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)g_inputCircle.dy);
    return S_OK;
}

// ------------------------------------------------------------
// Time / Random / System / App API
// ------------------------------------------------------------

static HRESULT Native_TimeMilliseconds(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)(osGetTime() & 0x7FFFFFFF));
    return S_OK;
}

static HRESULT Native_TimeSeconds(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)(osGetTime() / 1000));
    return S_OK;
}

static void EnsureRandomSeeded()
{
    if (g_randomSeed == 0)
    {
        g_randomSeed = (u32)osGetTime();

        if (g_randomSeed == 0)
        {
            g_randomSeed = 0x12345678;
        }
    }
}

static HRESULT Native_RandomSeed(CLR_RT_StackFrame& stack)
{
    int seed = stack.Arg0().NumericByRef().s4;
    g_randomSeed = (u32)seed;

    if (g_randomSeed == 0)
    {
        g_randomSeed = 0x12345678;
    }

    return S_OK;
}

static HRESULT Native_RandomNext(CLR_RT_StackFrame& stack)
{
    int min = stack.Arg0().NumericByRef().s4;
    int max = stack.Arg1().NumericByRef().s4;

    if (max <= min)
    {
        stack.SetResult_I4(min);
        return S_OK;
    }

    EnsureRandomSeeded();
    g_randomSeed = 1664525u * g_randomSeed + 1013904223u;

    u32 range = (u32)(max - min);
    int value = min + (int)(g_randomSeed % range);

    stack.SetResult_I4(value);
    return S_OK;
}

static HRESULT Native_AppGetPath(CLR_RT_StackFrame& stack)
{
    return stack.SetResult_String(g_selectedAppPath);
}

static HRESULT Native_AppGetDirectory(CLR_RT_StackFrame& stack)
{
    return stack.SetResult_String(g_selectedAppDir);
}

static HRESULT Native_AppGetName(CLR_RT_StackFrame& stack)
{
    return stack.SetResult_String(g_selectedAppName);
}

static HRESULT Native_SystemIsNew3DS(CLR_RT_StackFrame& stack)
{
    bool isNew3DS = false;
    Result rc = APT_CheckNew3DS(&isNew3DS);

    if (R_FAILED(rc))
    {
        isNew3DS = false;
    }

    stack.SetResult_I4(isNew3DS ? 1 : 0);
    return S_OK;
}

static HRESULT Native_SystemGetBatteryLevel(CLR_RT_StackFrame& stack)
{
    u8 level = 0;
    Result rc = ptmuInit();

    if (R_SUCCEEDED(rc))
    {
        rc = PTMU_GetBatteryLevel(&level);
        ptmuExit();
    }

    if (R_FAILED(rc))
    {
        stack.SetResult_I4(-1);
        return S_OK;
    }

    stack.SetResult_I4((int)level);
    return S_OK;
}

static HRESULT Native_SystemGetFreeMemory(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4((int)linearSpaceFree());
    return S_OK;
}

// ------------------------------------------------------------
// Graphics API
// ------------------------------------------------------------

static void DecodeColor(int color, u8& r, u8& g, u8& b)
{
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

static u8* GetBottomFramebuffer()
{
    u16 fbWidth = 0;
    u16 fbHeight = 0;

    return gfxGetFramebuffer(g_graphicsTarget, GFX_LEFT, &fbWidth, &fbHeight);
}

static void PutPixelBottomRaw(u8* fb, int x, int y, int color)
{
    if (fb == NULL)
    {
        return;
    }

    if (x < 0 || x >= GetGraphicsWidth() || y < 0 || y >= GetGraphicsHeight())
    {
        return;
    }

    u8 r, g, b;
    DecodeColor(color, r, g, b);

    int pos = ((239 - y) + x * 240) * 3;

    fb[pos + 0] = b;
    fb[pos + 1] = g;
    fb[pos + 2] = r;
}

static void PutPixelBottom(int x, int y, int color)
{
    u8* fb = GetBottomFramebuffer();
    PutPixelBottomRaw(fb, x, y, color);
}

static void FillRectBottom(int x, int y, int width, int height, int color)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    u8* fb = GetBottomFramebuffer();

    if (fb == NULL)
    {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GetGraphicsWidth()) x1 = GetGraphicsWidth();
    if (y1 > GetGraphicsHeight()) y1 = GetGraphicsHeight();

    for (int px = x0; px < x1; px++)
    {
        for (int py = y0; py < y1; py++)
        {
            PutPixelBottomRaw(fb, px, py, color);
        }
    }
}

static void DrawRectBottom(int x, int y, int width, int height, int color)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    for (int px = x; px < x + width; px++)
    {
        PutPixelBottom(px, y, color);
        PutPixelBottom(px, y + height - 1, color);
    }

    for (int py = y; py < y + height; py++)
    {
        PutPixelBottom(x, py, color);
        PutPixelBottom(x + width - 1, py, color);
    }
}

static void DrawLineBottom(int x0, int y0, int x1, int y1, int color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true)
    {
        PutPixelBottom(x0, y0, color);

        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int e2 = 2 * err;

        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

static void DrawCircleBottom(int centerX, int centerY, int radius, int color)
{
    if (radius < 0)
    {
        radius = -radius;
    }

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y)
    {
        PutPixelBottom(centerX + x, centerY + y, color);
        PutPixelBottom(centerX + y, centerY + x, color);
        PutPixelBottom(centerX - y, centerY + x, color);
        PutPixelBottom(centerX - x, centerY + y, color);
        PutPixelBottom(centerX - x, centerY - y, color);
        PutPixelBottom(centerX - y, centerY - x, color);
        PutPixelBottom(centerX + y, centerY - x, color);
        PutPixelBottom(centerX + x, centerY - y, color);

        y++;

        if (err <= 0)
        {
            err += 2 * y + 1;
        }

        if (err > 0)
        {
            x--;
            err -= 2 * x + 1;
        }
    }
}

static void FillCircleBottom(int centerX, int centerY, int radius, int color)
{
    if (radius < 0)
    {
        radius = -radius;
    }

    int radiusSquared = radius * radius;

    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            if (x * x + y * y <= radiusSquared)
            {
                PutPixelBottom(centerX + x, centerY + y, color);
            }
        }
    }
}

// ------------------------------------------------------------
// Small 5x7 bitmap font
// ------------------------------------------------------------

static const u8* GetGlyph5x7(char c)
{
    static const u8 SPACE[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    static const u8 A[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const u8 B[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const u8 C[7] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
    static const u8 D[7] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E };
    static const u8 E[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
    static const u8 F[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 };
    static const u8 G[7] = { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0E };
    static const u8 H[7] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const u8 I[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F };
    static const u8 J[7] = { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E };
    static const u8 K[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    static const u8 L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const u8 M[7] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 };
    static const u8 N[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
    static const u8 O[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    static const u8 P[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 };
    static const u8 Q[7] = { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D };
    static const u8 R[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const u8 S[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    static const u8 T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const u8 U[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    static const u8 V[7] = { 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 };
    static const u8 W[7] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A };
    static const u8 X[7] = { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11 };
    static const u8 Y[7] = { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const u8 Z[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F };

    static const u8 N0[7] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E };
    static const u8 N1[7] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E };
    static const u8 N2[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F };
    static const u8 N3[7] = { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E };
    static const u8 N4[7] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 };
    static const u8 N5[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E };
    static const u8 N6[7] = { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E };
    static const u8 N7[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 };
    static const u8 N8[7] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E };
    static const u8 N9[7] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E };

    static const u8 EXCL[7] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
    static const u8 QMARK[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
    static const u8 DOT[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };
    static const u8 COMMA[7] = { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08 };
    static const u8 COLON[7] = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 };
    static const u8 DASH[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
    static const u8 UNDERSCORE[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F };
    static const u8 GT[7] = { 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10 };
    static const u8 LT[7] = { 0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01 };

    if (c >= 'a' && c <= 'z')
    {
        c = c - 32;
    }

    switch (c)
    {
    case 'A': return A;
    case 'B': return B;
    case 'C': return C;
    case 'D': return D;
    case 'E': return E;
    case 'F': return F;
    case 'G': return G;
    case 'H': return H;
    case 'I': return I;
    case 'J': return J;
    case 'K': return K;
    case 'L': return L;
    case 'M': return M;
    case 'N': return N;
    case 'O': return O;
    case 'P': return P;
    case 'Q': return Q;
    case 'R': return R;
    case 'S': return S;
    case 'T': return T;
    case 'U': return U;
    case 'V': return V;
    case 'W': return W;
    case 'X': return X;
    case 'Y': return Y;
    case 'Z': return Z;

    case '0': return N0;
    case '1': return N1;
    case '2': return N2;
    case '3': return N3;
    case '4': return N4;
    case '5': return N5;
    case '6': return N6;
    case '7': return N7;
    case '8': return N8;
    case '9': return N9;

    case '!': return EXCL;
    case '?': return QMARK;
    case '.': return DOT;
    case ',': return COMMA;
    case ':': return COLON;
    case '-': return DASH;
    case '_': return UNDERSCORE;
    case '>': return GT;
    case '<': return LT;

    default: return SPACE;
    }
}

static void DrawGlyph5x7(int x, int y, const u8* glyph, int color)
{
    int scale = 2;

    for (int row = 0; row < 7; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            int bit = 1 << (4 - col);

            if (glyph[row] & bit)
            {
                FillRectBottom(
                    x + col * scale,
                    y + row * scale,
                    scale,
                    scale,
                    color
                );
            }
        }
    }
}

static void DrawTextBottom(int x, int y, const char* text, int color)
{
    if (text == NULL)
    {
        return;
    }

    int cursorX = x;
    int cursorY = y;

    for (int i = 0; text[i] != '\0'; i++)
    {
        char c = text[i];

        if (c == '\n')
        {
            cursorX = x;
            cursorY += 16;
            continue;
        }

        const u8* glyph = GetGlyph5x7(c);

        DrawGlyph5x7(cursorX, cursorY, glyph, color);

        cursorX += 12;
    }
}

static void ClearBottomBuffers(int color)
{
    FillRectBottom(0, 0, 320, 240, color);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    FillRectBottom(0, 0, 320, 240, color);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void ShowRuntimeLoadingScreen()
{
    ClearBottomBuffers(0x000000);

    DrawTextBottom(50, 90, "RESHARP3DS", 0xFFFFFF);
    DrawTextBottom(65, 115, "LOADING...", 0x808080);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    DrawTextBottom(50, 90, "RESHARP3DS", 0xFFFFFF);
    DrawTextBottom(65, 115, "LOADING...", 0x808080);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void ShowConsoleModeScreen()
{
    ClearBottomBuffers(0x000000);

    DrawTextBottom(42, 90, "CONSOLE MODE", 0xFFFFFF);
    DrawTextBottom(30, 118, "TOP SCREEN OUTPUT", 0x808080);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    DrawTextBottom(42, 90, "CONSOLE MODE", 0xFFFFFF);
    DrawTextBottom(30, 118, "TOP SCREEN OUTPUT", 0x808080);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void MarkGraphicsUsed()
{
    g_graphicsUsed = true;
    g_consoleModeScreenShown = true;
}

// ------------------------------------------------------------
// Graphics native calls
// ------------------------------------------------------------

static HRESULT Native_GraphicsSetTarget(CLR_RT_StackFrame& stack)
{
    int target = stack.Arg0().NumericByRef().s4;
    g_graphicsTarget = target == 1 ? GFX_TOP : GFX_BOTTOM;
    return S_OK;
}

static HRESULT Native_GraphicsClear(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int color = stack.Arg0().NumericByRef().s4;

    FillRectBottom(0, 0, 320, 240, color);

    return S_OK;
}

static HRESULT Native_GraphicsDrawPixel(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;
    int color = stack.Arg2().NumericByRef().s4;

    PutPixelBottom(x, y, color);

    return S_OK;
}

static HRESULT Native_GraphicsFillRect(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;
    int width = stack.Arg2().NumericByRef().s4;
    int height = stack.Arg3().NumericByRef().s4;
    int color = stack.Arg4().NumericByRef().s4;

    FillRectBottom(x, y, width, height, color);

    return S_OK;
}

static HRESULT Native_GraphicsDrawRect(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;
    int width = stack.Arg2().NumericByRef().s4;
    int height = stack.Arg3().NumericByRef().s4;
    int color = stack.Arg4().NumericByRef().s4;

    DrawRectBottom(x, y, width, height, color);

    return S_OK;
}

static HRESULT Native_GraphicsDrawText(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;

    CLR_RT_HeapBlock& textArg = stack.Arg2();
    const char* text = textArg.RecoverString();

    int color = stack.Arg3().NumericByRef().s4;

    DrawTextBottom(x, y, text, color);

    return S_OK;
}

static HRESULT Native_GraphicsDrawLine(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x0 = stack.Arg0().NumericByRef().s4;
    int y0 = stack.Arg1().NumericByRef().s4;
    int x1 = stack.Arg2().NumericByRef().s4;
    int y1 = stack.Arg3().NumericByRef().s4;
    int color = stack.Arg4().NumericByRef().s4;

    DrawLineBottom(x0, y0, x1, y1, color);

    return S_OK;
}

static HRESULT Native_GraphicsDrawCircle(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;
    int radius = stack.Arg2().NumericByRef().s4;
    int color = stack.Arg3().NumericByRef().s4;

    DrawCircleBottom(x, y, radius, color);

    return S_OK;
}

static HRESULT Native_GraphicsFillCircle(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    int x = stack.Arg0().NumericByRef().s4;
    int y = stack.Arg1().NumericByRef().s4;
    int radius = stack.Arg2().NumericByRef().s4;
    int color = stack.Arg3().NumericByRef().s4;

    FillCircleBottom(x, y, radius, color);

    return S_OK;
}

static HRESULT Native_GraphicsPresent(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    return S_OK;
}

static u16 ReadBmpU16LE(FILE* f)
{
    u8 b[2];
    if (fread(b, 1, 2, f) != 2) return 0;
    return ((u16)b[0]) | ((u16)b[1] << 8);
}

static u32 ReadBmpU32LE(FILE* f)
{
    u8 b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return ((u32)b[0]) | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

static s32 ReadBmpS32LE(FILE* f)
{
    return (s32)ReadBmpU32LE(f);
}

static HRESULT Native_GraphicsDrawBitmap(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    CLR_RT_HeapBlock& pathArg = stack.Arg0();
    const char* inputPath = pathArg.RecoverString();
    int drawX = stack.Arg1().NumericByRef().s4;
    int drawY = stack.Arg2().NumericByRef().s4;

    char resolvedPath[512];
    ResolveAppRelativePath(inputPath, resolvedPath, sizeof(resolvedPath));

    if (resolvedPath[0] == '\0')
    {
        printf("[BMP] path resolve failed\n");
        return S_OK;
    }

    FILE* f = fopen(resolvedPath, "rb");
    if (!f)
    {
        printf("[BMP] fopen failed: %s\n", resolvedPath);
        return S_OK;
    }

    u8 sig[2];
    if (fread(sig, 1, 2, f) != 2 || sig[0] != 'B' || sig[1] != 'M')
    {
        printf("[BMP] invalid signature\n");
        fclose(f);
        return S_OK;
    }

    ReadBmpU32LE(f);
    ReadBmpU16LE(f);
    ReadBmpU16LE(f);
    u32 pixelOffset = ReadBmpU32LE(f);
    u32 headerSize = ReadBmpU32LE(f);

    if (headerSize < 40)
    {
        printf("[BMP] unsupported header size=%lu\n", (unsigned long)headerSize);
        fclose(f);
        return S_OK;
    }

    s32 width = ReadBmpS32LE(f);
    s32 height = ReadBmpS32LE(f);
    u16 planes = ReadBmpU16LE(f);
    u16 bpp = ReadBmpU16LE(f);
    u32 compression = ReadBmpU32LE(f);

    if (planes != 1 || compression != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0)
    {
        printf("[BMP] unsupported format w=%ld h=%ld bpp=%u comp=%lu\n",
            (long)width,
            (long)height,
            (unsigned)bpp,
            (unsigned long)compression);
        fclose(f);
        return S_OK;
    }

    bool topDown = height < 0;
    if (height < 0) height = -height;

    int bytesPerPixel = bpp / 8;
    int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;

    for (int y = 0; y < height; y++)
    {
        int sourceY = topDown ? y : (height - 1 - y);
        long rowOffset = (long)pixelOffset + (long)sourceY * rowSize;

        if (fseek(f, rowOffset, SEEK_SET) != 0)
        {
            break;
        }

        for (int x = 0; x < width; x++)
        {
            u8 bgr[4];
            if (fread(bgr, 1, bytesPerPixel, f) != (size_t)bytesPerPixel)
            {
                break;
            }

            int color = ((int)bgr[2] << 16) | ((int)bgr[1] << 8) | (int)bgr[0];
            PutPixelBottom(drawX + x, drawY + y, color);
        }
    }

    fclose(f);
    return S_OK;
}

static HRESULT Native_GraphicsDrawSprite(CLR_RT_StackFrame& stack)
{
    return Native_GraphicsDrawBitmap(stack);
}

static HRESULT Native_GraphicsDrawSpriteTransparent(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    CLR_RT_HeapBlock& pathArg = stack.Arg0();
    const char* inputPath = pathArg.RecoverString();
    int drawX = stack.Arg1().NumericByRef().s4;
    int drawY = stack.Arg2().NumericByRef().s4;
    int transparentColor = stack.Arg3().NumericByRef().s4 & 0xFFFFFF;

    char resolvedPath[512];
    ResolveAppRelativePath(inputPath, resolvedPath, sizeof(resolvedPath));

    if (resolvedPath[0] == '\0')
    {
        printf("[BMP] path resolve failed\n");
        return S_OK;
    }

    FILE* f = fopen(resolvedPath, "rb");
    if (!f)
    {
        printf("[BMP] fopen failed: %s\n", resolvedPath);
        return S_OK;
    }

    u8 sig[2];
    if (fread(sig, 1, 2, f) != 2 || sig[0] != 'B' || sig[1] != 'M')
    {
        fclose(f);
        return S_OK;
    }

    ReadBmpU32LE(f);
    ReadBmpU16LE(f);
    ReadBmpU16LE(f);
    u32 pixelOffset = ReadBmpU32LE(f);
    u32 headerSize = ReadBmpU32LE(f);

    if (headerSize < 40)
    {
        fclose(f);
        return S_OK;
    }

    s32 width = ReadBmpS32LE(f);
    s32 height = ReadBmpS32LE(f);
    u16 planes = ReadBmpU16LE(f);
    u16 bpp = ReadBmpU16LE(f);
    u32 compression = ReadBmpU32LE(f);

    if (planes != 1 || compression != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0)
    {
        fclose(f);
        return S_OK;
    }

    bool topDown = height < 0;
    if (height < 0) height = -height;

    int bytesPerPixel = bpp / 8;
    int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;

    for (int y = 0; y < height; y++)
    {
        int sourceY = topDown ? y : (height - 1 - y);
        long rowOffset = (long)pixelOffset + (long)sourceY * rowSize;

        if (fseek(f, rowOffset, SEEK_SET) != 0)
        {
            break;
        }

        for (int x = 0; x < width; x++)
        {
            u8 bgr[4];
            if (fread(bgr, 1, bytesPerPixel, f) != (size_t)bytesPerPixel)
            {
                break;
            }

            int color = ((int)bgr[2] << 16) | ((int)bgr[1] << 8) | (int)bgr[0];
            if ((color & 0xFFFFFF) != transparentColor)
            {
                PutPixelBottom(drawX + x, drawY + y, color);
            }
        }
    }

    fclose(f);
    return S_OK;
}

static HRESULT Native_GraphicsDrawSpriteScaled(CLR_RT_StackFrame& stack)
{
    MarkGraphicsUsed();

    CLR_RT_HeapBlock& pathArg = stack.Arg0();
    const char* inputPath = pathArg.RecoverString();
    int drawX = stack.Arg1().NumericByRef().s4;
    int drawY = stack.Arg2().NumericByRef().s4;
    int targetWidth = stack.Arg3().NumericByRef().s4;
    int targetHeight = stack.Arg4().NumericByRef().s4;

    if (targetWidth <= 0 || targetHeight <= 0)
    {
        return S_OK;
    }

    char resolvedPath[512];
    ResolveAppRelativePath(inputPath, resolvedPath, sizeof(resolvedPath));

    if (resolvedPath[0] == '\0')
    {
        printf("[BMP] path resolve failed\n");
        return S_OK;
    }

    FILE* f = fopen(resolvedPath, "rb");
    if (!f)
    {
        printf("[BMP] fopen failed: %s\n", resolvedPath);
        return S_OK;
    }

    u8 sig[2];
    if (fread(sig, 1, 2, f) != 2 || sig[0] != 'B' || sig[1] != 'M')
    {
        printf("[BMP] invalid signature\n");
        fclose(f);
        return S_OK;
    }

    ReadBmpU32LE(f);
    ReadBmpU16LE(f);
    ReadBmpU16LE(f);
    u32 pixelOffset = ReadBmpU32LE(f);
    u32 headerSize = ReadBmpU32LE(f);

    if (headerSize < 40)
    {
        printf("[BMP] unsupported header size=%lu\n", (unsigned long)headerSize);
        fclose(f);
        return S_OK;
    }

    s32 width = ReadBmpS32LE(f);
    s32 height = ReadBmpS32LE(f);
    u16 planes = ReadBmpU16LE(f);
    u16 bpp = ReadBmpU16LE(f);
    u32 compression = ReadBmpU32LE(f);

    if (planes != 1 || compression != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0)
    {
        printf("[BMP] unsupported scaled format w=%ld h=%ld bpp=%u comp=%lu\n",
            (long)width,
            (long)height,
            (unsigned)bpp,
            (unsigned long)compression);
        fclose(f);
        return S_OK;
    }

    bool topDown = height < 0;
    if (height < 0) height = -height;

    int bytesPerPixel = bpp / 8;
    int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;

    for (int dy = 0; dy < targetHeight; dy++)
    {
        int sourceY = (dy * height) / targetHeight;
        int realSourceY = topDown ? sourceY : (height - 1 - sourceY);

        for (int dx = 0; dx < targetWidth; dx++)
        {
            int sourceX = (dx * width) / targetWidth;
            long pixelPos = (long)pixelOffset + (long)realSourceY * rowSize + (long)sourceX * bytesPerPixel;

            if (fseek(f, pixelPos, SEEK_SET) != 0)
            {
                continue;
            }

            u8 bgr[4];
            if (fread(bgr, 1, bytesPerPixel, f) != (size_t)bytesPerPixel)
            {
                continue;
            }

            int color = ((int)bgr[2] << 16) | ((int)bgr[1] << 8) | (int)bgr[0];
            PutPixelBottom(drawX + dx, drawY + dy, color);
        }
    }

    fclose(f);
    return S_OK;
}

// ------------------------------------------------------------
// Audio API
// ------------------------------------------------------------

static bool g_audioInitialized = false;
static float g_audioVolume = 1.0f;
static float g_audioSfxVolume = 1.0f;
static float g_audioMusicVolume = 1.0f;

static const int AUDIO_SFX_CHANNEL = 0;
static const int AUDIO_MUSIC_CHANNEL = 1;
static const int AUDIO_SAMPLE_RATE = 44100;
static const int AUDIO_MAX_DURATION_MS = 1000;
static const int AUDIO_MAX_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_MAX_DURATION_MS / 1000;


// Channel 0: beeps + one-shot WAV/SFX
static ndspWaveBuf g_beepWaveBuf;
static s16* g_audioBuffer = NULL;

static ndspWaveBuf g_wavWaveBuf;
static s16* g_wavBuffer = NULL;
static size_t g_wavBufferBytes = 0;

// Channel 1: looping music
static ndspWaveBuf g_musicWaveBuf;
static s16* g_musicBuffer = NULL;
static size_t g_musicBufferBytes = 0;

static float ClampVolumePercent(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return (float)volume / 100.0f;
}

static void ApplyAudioMix(int channel)
{
    float mix[12];
    memset(mix, 0, sizeof(mix));

    float volume = (channel == AUDIO_MUSIC_CHANNEL) ? g_audioMusicVolume : g_audioSfxVolume;

    mix[0] = volume;
    mix[1] = volume;

    ndspChnSetMix(channel, mix);
}

static void ConfigureAudioChannel(int channel, u32 sampleRate, u16 channels)
{
    ndspChnReset(channel);
    ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(channel, sampleRate);

    if (channels == 2)
    {
        ndspChnSetFormat(channel, NDSP_FORMAT_STEREO_PCM16);
    }
    else
    {
        ndspChnSetFormat(channel, NDSP_FORMAT_MONO_PCM16);
    }

    ApplyAudioMix(channel);
    ndspChnSetPaused(channel, false);
}

static void FreeSfxWavBuffer()
{
    if (g_wavBuffer)
    {
        linearFree(g_wavBuffer);
        g_wavBuffer = NULL;
        g_wavBufferBytes = 0;
    }
}

static void FreeMusicBuffer()
{
    if (g_musicBuffer)
    {
        linearFree(g_musicBuffer);
        g_musicBuffer = NULL;
        g_musicBufferBytes = 0;
    }
}

static HRESULT Native_AudioInit(CLR_RT_StackFrame& stack)
{
    if (g_audioInitialized)
    {
        return S_OK;
    }

    printf("[AUDIO] init\n");

    Result rc = ndspInit();

    if (R_FAILED(rc))
    {
        printf("[AUDIO] ndspInit failed: 0x%08lX\n", rc);
        return S_OK;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    ConfigureAudioChannel(AUDIO_SFX_CHANNEL, AUDIO_SAMPLE_RATE, 1);
    ConfigureAudioChannel(AUDIO_MUSIC_CHANNEL, AUDIO_SAMPLE_RATE, 2);

    memset(&g_beepWaveBuf, 0, sizeof(g_beepWaveBuf));
    memset(&g_wavWaveBuf, 0, sizeof(g_wavWaveBuf));
    memset(&g_musicWaveBuf, 0, sizeof(g_musicWaveBuf));

    g_audioBuffer = (s16*)linearAlloc(AUDIO_MAX_SAMPLES * sizeof(s16));

    if (!g_audioBuffer)
    {
        printf("[AUDIO] linearAlloc failed\n");
        ndspExit();
        return S_OK;
    }

    memset(g_audioBuffer, 0, AUDIO_MAX_SAMPLES * sizeof(s16));
    DSP_FlushDataCache(g_audioBuffer, AUDIO_MAX_SAMPLES * sizeof(s16));

    g_audioInitialized = true;

    printf("[AUDIO] ready\n");

    return S_OK;
}

static HRESULT Native_AudioSetVolume(CLR_RT_StackFrame& stack)
{
    int volume = stack.Arg0().NumericByRef().s4;

    g_audioVolume = ClampVolumePercent(volume);
    g_audioSfxVolume = g_audioVolume;
    g_audioMusicVolume = g_audioVolume;

    if (g_audioInitialized)
    {
        ApplyAudioMix(AUDIO_SFX_CHANNEL);
        ApplyAudioMix(AUDIO_MUSIC_CHANNEL);
    }

    printf("[AUDIO] volume=%d\n", volume);

    return S_OK;
}

static HRESULT Native_AudioSetSfxVolume(CLR_RT_StackFrame& stack)
{
    int volume = stack.Arg0().NumericByRef().s4;
    g_audioSfxVolume = ClampVolumePercent(volume);

    if (g_audioInitialized)
    {
        ApplyAudioMix(AUDIO_SFX_CHANNEL);
    }

    printf("[AUDIO] sfx volume=%d\n", volume);
    return S_OK;
}

static HRESULT Native_AudioSetMusicVolume(CLR_RT_StackFrame& stack)
{
    int volume = stack.Arg0().NumericByRef().s4;
    g_audioMusicVolume = ClampVolumePercent(volume);

    if (g_audioInitialized)
    {
        ApplyAudioMix(AUDIO_MUSIC_CHANNEL);
    }

    printf("[AUDIO] music volume=%d\n", volume);
    return S_OK;
}

static bool IsWaveBufActive(ndspWaveBuf& waveBuf)
{
    // libctru status values are usually: 0=free, 1=queued, 2=playing, 3=done.
    return waveBuf.status == 1 || waveBuf.status == 2;
}

static HRESULT Native_AudioIsPlaying(CLR_RT_StackFrame& stack)
{
    int playing = 0;

    if (g_audioInitialized)
    {
        playing = (IsWaveBufActive(g_beepWaveBuf) || IsWaveBufActive(g_wavWaveBuf)) ? 1 : 0;
    }

    stack.SetResult_I4(playing);
    return S_OK;
}

static HRESULT Native_AudioIsMusicPlaying(CLR_RT_StackFrame& stack)
{
    int playing = 0;

    if (g_audioInitialized)
    {
        playing = IsWaveBufActive(g_musicWaveBuf) ? 1 : 0;
    }

    stack.SetResult_I4(playing);
    return S_OK;
}

static HRESULT Native_AudioBeep(CLR_RT_StackFrame& stack)
{
    Native_AudioInit(stack);

    if (!g_audioInitialized || !g_audioBuffer)
    {
        printf("[AUDIO] beep ignored, audio not ready\n");
        return S_OK;
    }

    int frequency = stack.Arg0().NumericByRef().s4;
    int durationMs = stack.Arg1().NumericByRef().s4;

    if (frequency <= 0)
    {
        frequency = 440;
    }

    if (durationMs <= 0)
    {
        durationMs = 200;
    }

    if (durationMs > AUDIO_MAX_DURATION_MS)
    {
        durationMs = AUDIO_MAX_DURATION_MS;
    }

    int samples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;

    printf("[AUDIO] beep freq=%d duration=%d samples=%d\n",
        frequency,
        durationMs,
        samples
    );

    for (int i = 0; i < samples; i++)
    {
        double t = (double)i / (double)AUDIO_SAMPLE_RATE;
        double s = sin(2.0 * 3.141592653589793 * frequency * t);

        double fade = 1.0;

        if (i < 256)
        {
            fade = (double)i / 256.0;
        }
        else if (i > samples - 256)
        {
            fade = (double)(samples - i) / 256.0;
        }

        g_audioBuffer[i] = (s16)(s * fade * 18000.0);
    }

    for (int i = samples; i < AUDIO_MAX_SAMPLES; i++)
    {
        g_audioBuffer[i] = 0;
    }

    DSP_FlushDataCache(g_audioBuffer, samples * sizeof(s16));

    ndspChnWaveBufClear(AUDIO_SFX_CHANNEL);
    ConfigureAudioChannel(AUDIO_SFX_CHANNEL, AUDIO_SAMPLE_RATE, 1);

    memset(&g_beepWaveBuf, 0, sizeof(g_beepWaveBuf));
    g_beepWaveBuf.data_vaddr = g_audioBuffer;
    g_beepWaveBuf.nsamples = samples;
    g_beepWaveBuf.looping = false;

    ndspChnWaveBufAdd(AUDIO_SFX_CHANNEL, &g_beepWaveBuf);

    return S_OK;
}

static HRESULT Native_AudioStop(CLR_RT_StackFrame& stack)
{
    if (!g_audioInitialized)
    {
        return S_OK;
    }

    printf("[AUDIO] stop\n");

    ndspChnWaveBufClear(AUDIO_SFX_CHANNEL);
    FreeSfxWavBuffer();

    return S_OK;
}

static u32 ReadU32LE(FILE* f)
{
    u8 b[4];

    if (fread(b, 1, 4, f) != 4)
    {
        return 0;
    }

    return ((u32)b[0]) |
        ((u32)b[1] << 8) |
        ((u32)b[2] << 16) |
        ((u32)b[3] << 24);
}

static u16 ReadU16LE(FILE* f)
{
    u8 b[2];

    if (fread(b, 1, 2, f) != 2)
    {
        return 0;
    }

    return ((u16)b[0]) |
        ((u16)b[1] << 8);
}

static bool ReadFourCC(FILE* f, char out[4])
{
    return fread(out, 1, 4, f) == 4;
}

static bool FourCCEquals(const char id[4], const char* text)
{
    return id[0] == text[0] &&
        id[1] == text[1] &&
        id[2] == text[2] &&
        id[3] == text[3];
}

static bool LoadWavToChannel(
    const char* inputPath,
    int channel,
    bool looping,
    ndspWaveBuf& waveBuf,
    s16*& audioBuffer,
    size_t& audioBufferBytes,
    const char* label)
{
    if (inputPath == NULL || inputPath[0] == '\0')
    {
        printf("[AUDIO] %s path empty\n", label);
        return false;
    }

    char resolvedPath[512];
    ResolveAppRelativePath(inputPath, resolvedPath, sizeof(resolvedPath));

    if (resolvedPath[0] == '\0')
    {
        printf("[AUDIO] %s path resolve failed\n", label);
        return false;
    }

    printf("[AUDIO] play %s: %s\n", label, resolvedPath);

    FILE* f = fopen(resolvedPath, "rb");

    if (!f)
    {
        printf("[AUDIO] fopen %s failed\n", label);
        return false;
    }

    char riff[4];
    char wave[4];

    if (!ReadFourCC(f, riff))
    {
        fclose(f);
        printf("[AUDIO] invalid %s: no RIFF\n", label);
        return false;
    }

    ReadU32LE(f); // RIFF file size

    if (!ReadFourCC(f, wave))
    {
        fclose(f);
        printf("[AUDIO] invalid %s: no WAVE\n", label);
        return false;
    }

    if (!FourCCEquals(riff, "RIFF") || !FourCCEquals(wave, "WAVE"))
    {
        fclose(f);
        printf("[AUDIO] invalid %s header\n", label);
        return false;
    }

    bool foundFmt = false;
    bool foundData = false;

    u16 audioFormat = 0;
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 bitsPerSample = 0;

    u32 dataSize = 0;
    long dataOffset = 0;

    while (!foundData)
    {
        char chunkId[4];

        if (!ReadFourCC(f, chunkId))
        {
            break;
        }

        u32 chunkSize = ReadU32LE(f);
        long chunkStart = ftell(f);

        if (chunkStart < 0)
        {
            break;
        }

        if (FourCCEquals(chunkId, "fmt "))
        {
            audioFormat = ReadU16LE(f);
            channels = ReadU16LE(f);
            sampleRate = ReadU32LE(f);

            ReadU32LE(f); // byteRate
            ReadU16LE(f); // blockAlign

            bitsPerSample = ReadU16LE(f);

            foundFmt = true;
        }
        else if (FourCCEquals(chunkId, "data"))
        {
            dataOffset = ftell(f);
            dataSize = chunkSize;
            foundData = true;
            break;
        }

        fseek(f, chunkStart + chunkSize + (chunkSize & 1), SEEK_SET);
    }

    if (!foundFmt || !foundData)
    {
        fclose(f);
        printf("[AUDIO] %s missing fmt/data\n", label);
        return false;
    }

    if (audioFormat != 1)
    {
        fclose(f);
        printf("[AUDIO] unsupported %s format=%u, need PCM\n", label, audioFormat);
        return false;
    }

    if (channels != 1 && channels != 2)
    {
        fclose(f);
        printf("[AUDIO] unsupported %s channels=%u\n", label, channels);
        return false;
    }

    if (bitsPerSample != 16)
    {
        fclose(f);
        printf("[AUDIO] unsupported %s bits=%u, need 16\n", label, bitsPerSample);
        return false;
    }

    if (sampleRate != 44100 && sampleRate != 22050)
    {
        fclose(f);
        printf("[AUDIO] unsupported %s rate=%lu\n", label, sampleRate);
        return false;
    }

    if (dataSize == 0)
    {
        fclose(f);
        printf("[AUDIO] empty %s data\n", label);
        return false;
    }

    if (audioBuffer)
    {
        linearFree(audioBuffer);
        audioBuffer = NULL;
        audioBufferBytes = 0;
    }

    audioBuffer = (s16*)linearAlloc(dataSize);

    if (!audioBuffer)
    {
        fclose(f);
        printf("[AUDIO] %s linearAlloc failed, size=%lu\n", label, dataSize);
        return false;
    }

    fseek(f, dataOffset, SEEK_SET);

    size_t read = fread(audioBuffer, 1, dataSize, f);
    fclose(f);

    if (read != dataSize)
    {
        linearFree(audioBuffer);
        audioBuffer = NULL;
        audioBufferBytes = 0;
        printf("[AUDIO] %s fread failed\n", label);
        return false;
    }

    audioBufferBytes = dataSize;

    DSP_FlushDataCache(audioBuffer, dataSize);

    ndspChnWaveBufClear(channel);
    ConfigureAudioChannel(channel, sampleRate, channels);

    memset(&waveBuf, 0, sizeof(waveBuf));
    waveBuf.data_vaddr = audioBuffer;
    waveBuf.looping = looping;

    if (channels == 1)
    {
        waveBuf.nsamples = dataSize / sizeof(s16);
    }
    else
    {
        waveBuf.nsamples = dataSize / (sizeof(s16) * 2);
    }

    ndspChnWaveBufAdd(channel, &waveBuf);

    printf("[AUDIO] %s started samples=%lu rate=%lu channels=%u loop=%d\n",
        label,
        (u32)waveBuf.nsamples,
        sampleRate,
        channels,
        looping ? 1 : 0
    );

    return true;
}

static HRESULT Native_AudioPlayWav(CLR_RT_StackFrame& stack)
{
    Native_AudioInit(stack);

    if (!g_audioInitialized)
    {
        printf("[AUDIO] wav ignored, audio not ready\n");
        return S_OK;
    }

    CLR_RT_HeapBlock& pathArg = stack.Arg0();
    const char* inputPath = pathArg.RecoverString();

    LoadWavToChannel(
        inputPath,
        AUDIO_SFX_CHANNEL,
        false,
        g_wavWaveBuf,
        g_wavBuffer,
        g_wavBufferBytes,
        "wav"
    );

    return S_OK;
}

static HRESULT Native_AudioLoop(CLR_RT_StackFrame& stack)
{
    Native_AudioInit(stack);

    if (!g_audioInitialized)
    {
        printf("[AUDIO] loop ignored, audio not ready\n");
        return S_OK;
    }

    CLR_RT_HeapBlock& pathArg = stack.Arg0();
    const char* inputPath = pathArg.RecoverString();

    LoadWavToChannel(
        inputPath,
        AUDIO_MUSIC_CHANNEL,
        g_musicLoopEnabled,
        g_musicWaveBuf,
        g_musicBuffer,
        g_musicBufferBytes,
        "music"
    );

    return S_OK;
}

static HRESULT Native_AudioPauseMusic(CLR_RT_StackFrame& stack)
{
    if (!g_audioInitialized)
    {
        return S_OK;
    }

    ndspChnSetPaused(AUDIO_MUSIC_CHANNEL, true);
    g_musicPaused = true;
    printf("[AUDIO] pause music\n");

    return S_OK;
}

static HRESULT Native_AudioResumeMusic(CLR_RT_StackFrame& stack)
{
    if (!g_audioInitialized)
    {
        return S_OK;
    }

    ndspChnSetPaused(AUDIO_MUSIC_CHANNEL, false);
    g_musicPaused = false;
    printf("[AUDIO] resume music\n");

    return S_OK;
}

static HRESULT Native_AudioSetMusicLoop(CLR_RT_StackFrame& stack)
{
    int loop = stack.Arg0().NumericByRef().s4;

    g_musicLoopEnabled = loop != 0;
    g_musicWaveBuf.looping = g_musicLoopEnabled;

    printf("[AUDIO] music loop=%d\n", g_musicLoopEnabled ? 1 : 0);

    return S_OK;
}

static HRESULT Native_AudioPlaySfx(CLR_RT_StackFrame& stack)
{
    return Native_AudioPlayWav(stack);
}

static HRESULT Native_AudioStopMusic(CLR_RT_StackFrame& stack)
{
    if (!g_audioInitialized)
    {
        return S_OK;
    }

    printf("[AUDIO] stop music\n");

    ndspChnWaveBufClear(AUDIO_MUSIC_CHANNEL);
    g_musicPaused = false;
    FreeMusicBuffer();

    return S_OK;
}

static void AudioShutdown()
{
    if (g_audioInitialized)
    {
        ndspChnWaveBufClear(AUDIO_SFX_CHANNEL);
        ndspChnWaveBufClear(AUDIO_MUSIC_CHANNEL);
    }

    FreeSfxWavBuffer();
    FreeMusicBuffer();

    if (g_audioBuffer)
    {
        linearFree(g_audioBuffer);
        g_audioBuffer = NULL;
    }

    if (g_audioInitialized)
    {
        ndspExit();
        g_audioInitialized = false;
    }
}

// ------------------------------------------------------------
// File API
// ------------------------------------------------------------

static const int FILE_READ_MAX_SIZE = 4096;
static char g_fileReadBuffer[FILE_READ_MAX_SIZE];

static CLR_RT_HeapBlock& GetStackArg(CLR_RT_StackFrame& stack, int argIndex)
{
    if (argIndex == 0)
    {
        return stack.Arg0();
    }

    if (argIndex == 1)
    {
        return stack.Arg1();
    }

    if (argIndex == 2)
    {
        return stack.Arg2();
    }

    if (argIndex == 3)
    {
        return stack.Arg3();
    }

    return stack.Arg0();
}

static bool ResolveFilePathFromManagedString(CLR_RT_StackFrame& stack, int argIndex, char* outPath, size_t outSize)
{
    if (outPath == NULL || outSize == 0)
    {
        return false;
    }

    outPath[0] = '\0';

    CLR_RT_HeapBlock& pathArg = GetStackArg(stack, argIndex);
    const char* inputPath = pathArg.RecoverString();

    if (inputPath == NULL || inputPath[0] == '\0')
    {
        printf("[FILE] empty path\n");
        return false;
    }

    ResolveAppRelativePath(inputPath, outPath, outSize);

    if (outPath[0] == '\0')
    {
        printf("[FILE] path resolve failed\n");
        return false;
    }

    return true;
}

static HRESULT Native_FileExists(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        stack.SetResult_I4(0);
        return S_OK;
    }

    struct stat st;

    bool exists = stat(path, &st) == 0;

    if (exists && S_ISDIR(st.st_mode))
    {
        exists = false;
    }

    printf("[FILE] exists %s = %d\n", path, exists ? 1 : 0);

    stack.SetResult_I4(exists ? 1 : 0);

    return S_OK;
}

static HRESULT Native_FileWriteAllText(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return S_OK;
    }

    CLR_RT_HeapBlock& textArg = stack.Arg1();
    const char* text = textArg.RecoverString();

    if (text == NULL)
    {
        text = "";
    }

    FILE* f = fopen(path, "wb");

    if (!f)
    {
        printf("[FILE] write failed: %s\n", path);
        return S_OK;
    }

    fwrite(text, 1, strlen(text), f);
    fclose(f);

    printf("[FILE] wrote text: %s\n", path);

    return S_OK;
}

static HRESULT Native_FileReadAllText(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return stack.SetResult_String("");
    }

    FILE* f = fopen(path, "rb");

    if (!f)
    {
        printf("[FILE] read failed: %s\n", path);
        return stack.SetResult_String("");
    }

    size_t read = fread(g_fileReadBuffer, 1, FILE_READ_MAX_SIZE - 1, f);
    fclose(f);

    g_fileReadBuffer[read] = '\0';

    printf("[FILE] read text: %s bytes=%u\n", path, (unsigned)read);

    return stack.SetResult_String(g_fileReadBuffer);
}

static HRESULT Native_FileDelete(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return S_OK;
    }

    int result = remove(path);

    if (result == 0)
    {
        printf("[FILE] deleted: %s\n", path);
    }
    else
    {
        printf("[FILE] delete failed: %s errno=%d\n", path, errno);
    }

    return S_OK;
}

static HRESULT Native_FileGetSize(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        stack.SetResult_I4(-1);
        return S_OK;
    }

    struct stat st;

    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
    {
        stack.SetResult_I4(-1);
        return S_OK;
    }

    stack.SetResult_I4((int)st.st_size);

    return S_OK;
}

static const int DIRECTORY_LIST_MAX_SIZE = 4096;
static char g_directoryListBuffer[DIRECTORY_LIST_MAX_SIZE];

static HRESULT BuildDirectoryList(CLR_RT_StackFrame& stack, bool wantFolders)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return stack.SetResult_String("");
    }

    g_directoryListBuffer[0] = '\0';

    DIR* dir = opendir(path);

    if (!dir)
    {
        printf("[DIR] list open failed: %s errno=%d\n", path, errno);
        return stack.SetResult_String("");
    }

    size_t used = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL)
    {
        const char* name = entry->d_name;

        if (!name || name[0] == '\0')
        {
            continue;
        }

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        {
            continue;
        }

        char fullPath[768];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, name);

        struct stat st;
        if (stat(fullPath, &st) != 0)
        {
            continue;
        }

        bool isDir = S_ISDIR(st.st_mode);

        if (wantFolders != isDir)
        {
            continue;
        }

        size_t nameLen = strlen(name);

        if (used + nameLen + 2 >= DIRECTORY_LIST_MAX_SIZE)
        {
            break;
        }

        memcpy(g_directoryListBuffer + used, name, nameLen);
        used += nameLen;
        g_directoryListBuffer[used++] = '\n';
        g_directoryListBuffer[used] = '\0';
    }

    closedir(dir);

    return stack.SetResult_String(g_directoryListBuffer);
}

static HRESULT Native_DirectoryListFiles(CLR_RT_StackFrame& stack)
{
    return BuildDirectoryList(stack, false);
}

static HRESULT Native_DirectoryListFolders(CLR_RT_StackFrame& stack)
{
    return BuildDirectoryList(stack, true);
}

static HRESULT Native_DirectoryExists(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        stack.SetResult_I4(0);
        return S_OK;
    }

    struct stat st;

    bool exists = stat(path, &st) == 0 && S_ISDIR(st.st_mode);

    printf("[DIR] exists %s = %d\n", path, exists ? 1 : 0);

    stack.SetResult_I4(exists ? 1 : 0);

    return S_OK;
}

static HRESULT Native_DirectoryCreate(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return S_OK;
    }

    int result = mkdir(path, 0777);

    if (result == 0)
    {
        printf("[DIR] created: %s\n", path);
    }
    else if (errno == EEXIST)
    {
        printf("[DIR] already exists: %s\n", path);
    }
    else
    {
        printf("[DIR] create failed: %s errno=%d\n", path, errno);
    }

    return S_OK;
}

static HRESULT Native_DirectoryDelete(CLR_RT_StackFrame& stack)
{
    char path[512];

    if (!ResolveFilePathFromManagedString(stack, 0, path, sizeof(path)))
    {
        return S_OK;
    }

    int result = rmdir(path);

    if (result == 0)
    {
        printf("[DIR] deleted: %s\n", path);
    }
    else
    {
        printf("[DIR] delete failed: %s errno=%d\n", path, errno);
    }

    return S_OK;
}

// ------------------------------------------------------------
// Runtime API
// ------------------------------------------------------------

static HRESULT Native_Yield(CLR_RT_StackFrame& stack)
{
    gspWaitForVBlank();
    return S_OK;
}

static HRESULT Native_DebugLogFile(CLR_RT_StackFrame& stack)
{
    CLR_RT_HeapBlock& levelArg = stack.Arg0();
    CLR_RT_HeapBlock& textArg = stack.Arg1();

    const char* level = levelArg.RecoverString();
    const char* text = textArg.RecoverString();

    AppendRuntimeLog(level, text);
    return S_OK;
}

static HRESULT Native_RuntimeExit(CLR_RT_StackFrame& stack)
{
    g_runtimeExitRequested = true;
    return S_OK;
}

static HRESULT Native_RuntimeRestart(CLR_RT_StackFrame& stack)
{
    g_runtimeRestartRequested = true;
    g_runtimeExitRequested = true;
    return S_OK;
}

static HRESULT Native_RuntimeReturnToLauncher(CLR_RT_StackFrame& stack)
{
    g_runtimeRestartRequested = false;
    g_runtimeExitRequested = true;
    return S_OK;
}

static HRESULT Native_RuntimeGetVersion(CLR_RT_StackFrame& stack)
{
    return stack.SetResult_String(RUNTIME_VERSION);
}

static HRESULT Native_RuntimeGetFps(CLR_RT_StackFrame& stack)
{
    stack.SetResult_I4(g_runtimeFps);
    return S_OK;
}

static void UpdateRuntimeFps()
{
    u64 now = osGetTime();

    if (g_runtimeFpsLastTime == 0)
    {
        g_runtimeFpsLastTime = now;
        g_runtimeFrameCounter = 0;
        g_runtimeFps = 0;
        return;
    }

    g_runtimeFrameCounter++;

    if (now - g_runtimeFpsLastTime >= 1000)
    {
        g_runtimeFps = (int)g_runtimeFrameCounter;
        g_runtimeFrameCounter = 0;
        g_runtimeFpsLastTime = now;
    }
}

// ------------------------------------------------------------
// Dynamic native table app.pe
// ------------------------------------------------------------

struct AppNativeBinding
{
    const char* methodName;
    CLR_RT_MethodHandler handler;
};

static AppNativeBinding g_appNativeBindings[] =
{
    // Console
    { "Clear", Native_Clear },
    { "Write", Native_Write },
    { "WriteLine", Native_WriteLine },
    { "WriteInt", Native_WriteInt },
    { "WriteLineInt", Native_WriteLineInt },

    // Input
    { "IsStartPressed", Native_IsStartPressed },
    { "IsSelectPressed", Native_IsSelectPressed },
    { "IsAPressed", Native_IsAPressed },
    { "IsBPressed", Native_IsBPressed },
    { "IsXPressed", Native_IsXPressed },
    { "IsYPressed", Native_IsYPressed },
    { "IsLPressed", Native_IsLPressed },
    { "IsRPressed", Native_IsRPressed },
    { "IsUpPressed", Native_IsUpPressed },
    { "IsDownPressed", Native_IsDownPressed },
    { "IsLeftPressed", Native_IsLeftPressed },
    { "IsRightPressed", Native_IsRightPressed },
    { "IsStartPressedOnce", Native_IsStartPressedOnce },
    { "IsSelectPressedOnce", Native_IsSelectPressedOnce },
    { "IsAPressedOnce", Native_IsAPressedOnce },
    { "IsBPressedOnce", Native_IsBPressedOnce },
    { "IsXPressedOnce", Native_IsXPressedOnce },
    { "IsYPressedOnce", Native_IsYPressedOnce },
    { "IsLPressedOnce", Native_IsLPressedOnce },
    { "IsRPressedOnce", Native_IsRPressedOnce },
    { "IsUpPressedOnce", Native_IsUpPressedOnce },
    { "IsDownPressedOnce", Native_IsDownPressedOnce },
    { "IsLeftPressedOnce", Native_IsLeftPressedOnce },
    { "IsRightPressedOnce", Native_IsRightPressedOnce },
    { "TouchIsPressed", Native_TouchIsPressed },
    { "TouchX", Native_TouchX },
    { "TouchY", Native_TouchY },
    { "CirclePadX", Native_CirclePadX },
    { "CirclePadY", Native_CirclePadY },

    // Time / Random / System / App
    { "TimeMilliseconds", Native_TimeMilliseconds },
    { "TimeSeconds", Native_TimeSeconds },
    { "RandomSeed", Native_RandomSeed },
    { "RandomNext", Native_RandomNext },
    { "AppGetPath", Native_AppGetPath },
    { "AppGetDirectory", Native_AppGetDirectory },
    { "AppGetName", Native_AppGetName },
    { "SystemIsNew3DS", Native_SystemIsNew3DS },
    { "SystemGetBatteryLevel", Native_SystemGetBatteryLevel },
    { "SystemGetFreeMemory", Native_SystemGetFreeMemory },
    { "DebugLogFile", Native_DebugLogFile },

    // Graphics
    { "GraphicsSetTarget", Native_GraphicsSetTarget },
    { "GraphicsClear", Native_GraphicsClear },
    { "GraphicsDrawPixel", Native_GraphicsDrawPixel },
    { "GraphicsFillRect", Native_GraphicsFillRect },
    { "GraphicsDrawRect", Native_GraphicsDrawRect },
    { "GraphicsDrawText", Native_GraphicsDrawText },
    { "GraphicsDrawBitmap", Native_GraphicsDrawBitmap },
    { "GraphicsDrawSprite", Native_GraphicsDrawSprite },
    { "GraphicsDrawSpriteScaled", Native_GraphicsDrawSpriteScaled },
    { "GraphicsDrawSpriteTransparent", Native_GraphicsDrawSpriteTransparent },
    { "GraphicsDrawLine", Native_GraphicsDrawLine },
    { "GraphicsDrawCircle", Native_GraphicsDrawCircle },
    { "GraphicsFillCircle", Native_GraphicsFillCircle },
    { "GraphicsPresent", Native_GraphicsPresent },

    // Audio
    { "AudioInit", Native_AudioInit },
    { "AudioBeep", Native_AudioBeep },
    { "AudioStop", Native_AudioStop },
    { "AudioPlayWav", Native_AudioPlayWav },
    { "AudioSetVolume", Native_AudioSetVolume },
    { "AudioSetSfxVolume", Native_AudioSetSfxVolume },
    { "AudioSetMusicVolume", Native_AudioSetMusicVolume },
    { "AudioIsPlaying", Native_AudioIsPlaying },
    { "AudioIsMusicPlaying", Native_AudioIsMusicPlaying },
    { "AudioLoop", Native_AudioLoop },
    { "AudioStopMusic", Native_AudioStopMusic },
    { "AudioPauseMusic", Native_AudioPauseMusic },
    { "AudioResumeMusic", Native_AudioResumeMusic },
    { "AudioSetMusicLoop", Native_AudioSetMusicLoop },
    { "AudioPlaySfx", Native_AudioPlaySfx },
    { "SetVolume", Native_AudioSetVolume },
    { "SetSfxVolume", Native_AudioSetSfxVolume },
    { "SetMusicVolume", Native_AudioSetMusicVolume },
    { "Loop", Native_AudioLoop },
    { "StopMusic", Native_AudioStopMusic },
    { "PauseMusic", Native_AudioPauseMusic },
    { "ResumeMusic", Native_AudioResumeMusic },
    { "SetMusicLoop", Native_AudioSetMusicLoop },
    { "PlaySfx", Native_AudioPlaySfx },

    // File
    { "FileExists", Native_FileExists },
    { "FileWriteAllText", Native_FileWriteAllText },
    { "FileReadAllText", Native_FileReadAllText },
    { "FileDelete", Native_FileDelete },
    { "FileGetSize", Native_FileGetSize },

    // Directory
    { "DirectoryExists", Native_DirectoryExists },
    { "DirectoryCreate", Native_DirectoryCreate },
    { "DirectoryDelete", Native_DirectoryDelete },
    { "DirectoryListFiles", Native_DirectoryListFiles },
    { "DirectoryListFolders", Native_DirectoryListFolders },

    // Runtime
    { "Yield", Native_Yield },
    { "RuntimeExit", Native_RuntimeExit },
    { "RuntimeRestart", Native_RuntimeRestart },
    { "RuntimeGetVersion", Native_RuntimeGetVersion },
    { "RuntimeGetFps", Native_RuntimeGetFps },
    { "RuntimeReturnToLauncher", Native_RuntimeReturnToLauncher },

    { NULL, NULL }
};

static CLR_RT_MethodHandler FindAppNativeHandlerByName(const char* methodName)
{
    if (methodName == NULL)
    {
        return NULL;
    }

    for (int i = 0; g_appNativeBindings[i].methodName != NULL; i++)
    {
        if (strcmp(methodName, g_appNativeBindings[i].methodName) == 0)
        {
            return g_appNativeBindings[i].handler;
        }
    }

    return NULL;
}

static bool IsNativeExternMethod(const CLR_RECORD_METHODDEF* methodDef)
{
    if (methodDef == NULL)
    {
        return false;
    }

    // InternalCall/native extern methods have no IL body.
    // User methods like PlayNote, Update, Draw, etc. have an RVA and are ignored.
    return methodDef->RVA == CLR_EmptyIndex;
}

static void InitAppNativeTable()
{
    for (int i = 0; i < 1024; i++)
    {
        g_appNativeMethods[i] = NULL;
    }

    if (g_appAssembly == NULL)
    {
        printf("[PATCH] ERROR: g_appAssembly is NULL\n");
        return;
    }

    CLR_IDX entryMethod = g_CLR_RT_TypeSystem.m_entryPoint.Method();
    int methodCount = g_appAssembly->m_pTablesSize[TBL_MethodDef];
    int installedCount = 0;

    printf("[PATCH] app entryMethod=%u\n", (unsigned)entryMethod);
    printf("[PATCH] dynamic native mapping start, methodCount=%d\n", methodCount);

    for (int methodIndex = 0; methodIndex < methodCount && methodIndex < 1024; methodIndex++)
    {
        const CLR_RECORD_METHODDEF* methodDef = g_appAssembly->GetMethodDef((CLR_IDX)methodIndex);

        if (methodDef == NULL)
        {
            continue;
        }

        const char* methodName = g_appAssembly->GetString(methodDef->name);
        CLR_RT_MethodHandler handler = FindAppNativeHandlerByName(methodName);

        if (handler == NULL)
        {
            continue;
        }

        if (!IsNativeExternMethod(methodDef))
        {
            printf("[PATCH] skip non-native method %s at method=%d\n",
                methodName ? methodName : "NULL",
                methodIndex);
            continue;
        }

        if ((CLR_IDX)methodIndex == entryMethod)
        {
            printf("[PATCH] ERROR: native %s conflicts with Program.Main at method=%d\n",
                methodName ? methodName : "NULL",
                methodIndex);
            continue;
        }

        g_appNativeMethods[methodIndex] = handler;
        installedCount++;

        printf("[PATCH] %s installed dynamically at method=%d\n",
            methodName ? methodName : "NULL",
            methodIndex);
    }

    g_appAssembly->m_nativeCode = g_appNativeMethods;

    printf("[PATCH] app native API table installed dynamically, count=%d\n", installedCount);
}

// ------------------------------------------------------------
// Load PE
// ------------------------------------------------------------

static CLR_RT_Assembly* LoadAssembly(const char* path)
{
    printf("[LOAD] %s\n", path);

    FILE* f = fopen(path, "rb");

    if (!f)
    {
        printf("[ERR] fopen %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        printf("[ERR] invalid size %s\n", path);
        fclose(f);
        return NULL;
    }

    unsigned char* buf = (unsigned char*)memalign(32, size);

    if (!buf)
    {
        printf("[ERR] alloc %s\n", path);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);

    if (read != (size_t)size)
    {
        printf("[ERR] fread %s\n", path);
        return NULL;
    }

    const CLR_RECORD_ASSEMBLY* hdr = (const CLR_RECORD_ASSEMBLY*)buf;

    CLR_RT_Assembly* asmObj = NULL;

    HRESULT hr = CLR_RT_Assembly::CreateInstance(hdr, asmObj);

    if (!SUCCEEDED(hr))
    {
        printf("[ERR] CreateInstance %s = 0x%08X\n", path, (unsigned)hr);
        return NULL;
    }

    g_CLR_RT_TypeSystem.Link(asmObj);

    printf("[OK] linked %s\n", path);
    printf("[ASM] name=%s idx=%u methodCount=%d\n",
        asmObj->m_szName ? asmObj->m_szName : "NULL",
        (unsigned)asmObj->m_idx,
        asmObj->m_pTablesSize[TBL_MethodDef]);

    return asmObj;
}

static void WaitExit()
{
    printf("\nSTART to exit\n");

    while (aptMainLoop())
    {
        hidScanInput();

        if (hidKeysDown() & KEY_START)
        {
            break;
        }

        gspWaitForVBlank();
    }
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------

int main()
{
    gfxInitDefault();

    bool start_runtime = false;
    char selectedAppPath[256];
    selectedAppPath[0] = '\0';

    while (aptMainLoop())
    {
        run_gui();

        hidScanInput();

        u32 kDown = hidKeysDown();

        if (kDown & KEY_DUP)
        {
            gui_move_selection(-1);
        }

        if (kDown & KEY_DDOWN)
        {
            gui_move_selection(1);
        }

        if (kDown & KEY_X)
        {
            gui_refresh_files();
        }

        if (kDown & KEY_TOUCH)
        {
            touchPosition touch;
            hidTouchRead(&touch);

            if (gui_handle_touch((int)touch.px, (int)touch.py))
            {
                if (gui_can_launch())
                {
                    const char* selected = gui_get_selected_app_path();

                    if (selected != NULL)
                    {
                        snprintf(selectedAppPath, sizeof(selectedAppPath), "%s", selected);
                        SetSelectedAppDirectory(selectedAppPath);
                        start_runtime = true;
                        break;
                    }
                }
            }
        }

        if (kDown & KEY_A)
        {
            if (gui_can_launch())
            {
                const char* selected = gui_get_selected_app_path();

                if (selected != NULL)
                {
                    snprintf(selectedAppPath, sizeof(selectedAppPath), "%s", selected);
                    SetSelectedAppDirectory(selectedAppPath);
                    start_runtime = true;
                    break;
                }
            }
        }

        if (kDown & KEY_START)
        {
            break;
        }

        gspWaitForVBlank();
    }

    gui_shutdown();

    if (!start_runtime)
    {
        gfxExit();
        return 0;
    }

    g_graphicsUsed = false;
    g_consoleModeScreenShown = false;

    ShowRuntimeLoadingScreen();

    consoleInit(GFX_TOP, NULL);
    consoleClear();

    printf("=== ReSharp3DS Runtime ===\n\n");
    printf("[APP] selected=%s\n", selectedAppPath);


    CLR_SETTINGS settings;
    memset(&settings, 0, sizeof(settings));

    HRESULT hr = nanoCLR_Initialize(&settings);
    if (!SUCCEEDED(hr))
    {
        printf("[ERR INIT] 0x%08X\n", (unsigned)hr);
        WaitExit();
        AudioShutdown();
        nanoCLR_Cleanup();
        gfxExit();
        return 0;
    }

    g_mscorlibAssembly = LoadAssembly("sdmc:/ReSharp3DS/bin/mscorlib.pe");

    if (!g_mscorlibAssembly)
    {
        printf("[FATAL] mscorlib load failed\n");
        WaitExit();
        AudioShutdown();
        nanoCLR_Cleanup();
        gfxExit();
        return 0;
    }

    g_appAssembly = LoadAssembly(selectedAppPath);

    if (!g_appAssembly)
    {
        printf("[FATAL] app load failed\n");
        WaitExit();
        AudioShutdown();
        nanoCLR_Cleanup();
        gfxExit();
        return 0;
    }

    printf("[DEBUG] ResolveAll...\n");
    hr = g_CLR_RT_TypeSystem.ResolveAll();
    printf("[ResolveAll] 0x%08X\n", (unsigned)hr);

    if (!SUCCEEDED(hr))
    {
        printf("[FATAL] ResolveAll failed\n");
        WaitExit();
        AudioShutdown();
        nanoCLR_Cleanup();
        gfxExit();
        return 0;
    }

    printf("[DEBUG] PrepareForExecution...\n");
    hr = g_CLR_RT_TypeSystem.PrepareForExecution();
    printf("[Prepare] 0x%08X\n", (unsigned)hr);

    printf("[DEBUG] entryPoint data=0x%08X method=%u assembly=%u\n",
        (unsigned)g_CLR_RT_TypeSystem.m_entryPoint.m_data,
        (unsigned)g_CLR_RT_TypeSystem.m_entryPoint.Method(),
        (unsigned)g_CLR_RT_TypeSystem.m_entryPoint.Assembly());

    if (!SUCCEEDED(hr))
    {
        printf("[FATAL] PrepareForExecution failed\n");
        WaitExit();
        AudioShutdown();
        nanoCLR_Cleanup();
        gfxExit();
        return 0;
    }

    InitMscorlibNativeTable();
    InitAppNativeTable();

    static wchar_t args[] = L"";

    printf("[CLR] Execute\n");

    while (aptMainLoop())
    {
        UpdateRuntimeInputSnapshot();
        hr = g_CLR_RT_ExecutionEngine.Execute(args, 1000);

        if (!SUCCEEDED(hr))
        {
            printf("[CLR] ERROR 0x%08X\n", (unsigned)hr);
            break;
        }

        UpdateRuntimeFps();

        if (g_runtimeExitRequested)
        {
            printf("[CLR] runtime exit requested\n");
            break;
        }

        if (!g_graphicsUsed && !g_consoleModeScreenShown)
        {
            ShowConsoleModeScreen();
            g_consoleModeScreenShown = true;
        }
    }

    printf("[CLR] loop ended\n");

    WaitExit();

    AudioShutdown();
    nanoCLR_Cleanup();
    gfxExit();

    if (g_runtimeRestartRequested)
    {
        return main();
    }

    return 0;
}