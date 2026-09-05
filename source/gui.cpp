#include "gui.h"
#include "runtime_net.h"

#include <3ds.h>
#include <citro2d.h>

#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------
// Config
// ------------------------------------------------------------

static const char* APP_VERSION = "v2.2.5-release";

static const char* ROOT_PATH = "sdmc:/ReSharp3DS";
static const char* BIN_PATH = "sdmc:/ReSharp3DS/bin";
static const char* MSCORLIB_PATH = "sdmc:/ReSharp3DS/bin/mscorlib.pe";
static const char* MSCORLIB_URL = "https://github.com/saysaa/ReSharp3DS/releases/latest/download/mscorlib.pe";
static const char* MSCORLIB_FALLBACK_URL = "https://raw.githubusercontent.com/saysaa/ReSharp3DS/sdk/bin/mscorlib.pe";


// ------------------------------------------------------------
// Browser state
// ------------------------------------------------------------

#define MAX_BROWSER_ITEMS 64
#define MAX_ITEM_NAME 128
#define MAX_ITEM_PATH 256

enum BrowserItemType
{
    ITEM_PARENT = 0,
    ITEM_DIRECTORY = 1,
    ITEM_APP = 2
};

struct BrowserItem
{
    char name[MAX_ITEM_NAME];
    char displayName[MAX_ITEM_NAME];
    char author[64];
    char version[32];
    char path[MAX_ITEM_PATH];
    BrowserItemType type;
};

static C3D_RenderTarget* topScreen = nullptr;
static C3D_RenderTarget* bottomScreen = nullptr;
static C2D_TextBuf textBuf = nullptr;

static bool initialized = false;
static bool folderFound = false;
static bool mscorlibFound = false;
// static bool latestFound = false;
static bool canLaunch = false;

static BrowserItem browserItems[MAX_BROWSER_ITEMS];
static int browserItemCount = 0;
static int selectedIndex = 0;
static int listOffset = 0;

static char selectedAppPath[MAX_ITEM_PATH];
static char currentDir[MAX_ITEM_PATH];

static char statusText[128];
static u64 statusTextTime = 0;
static bool statusTextTemporary = false;


// ------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------

static void draw_text(const char* text, float x, float y, float scale, u32 color);
static void draw_text_centered(const char* text, float screenWidth, float y, float scale, u32 color);

// ------------------------------------------------------------
// Status helpers
// ------------------------------------------------------------

static int GetSystemLanguage()
{
    u8 language = 0;
    Result res;

    res = CFGU_GetSystemLanguage(&language);
    /* 1 = english, 2 = french, 3 = german, 4 = italian, 5 = japanese,
    6 = spanish, 7 = korean, 8 = dutch, 9 = portuguese (brazil),
    10 = russian, 11 = chinese (simplified), 12 = chinese (traditional)*/

    // Error handling: if the function fails, return -1.
    if (res != 0)
    {
        return -1;
    }

    return (int)language;
}

static void set_status_text(const char* text, bool temporary)
{
    if (text == NULL)
    {
        text = "";
    }

    snprintf(statusText, sizeof(statusText), "%s", text);

    statusTextTemporary = temporary;
    statusTextTime = osGetTime();
}

static void update_status_text_timer(void)
{
    if (!statusTextTemporary)
    {
        return;
    }

    u64 now = osGetTime();

    if (now - statusTextTime >= 3000)
    {
        statusText[0] = '\0';
        statusTextTemporary = false;
    }
}

// ------------------------------------------------------------
// Runtime file helpers
// ------------------------------------------------------------

static bool ensure_runtime_files(void)
{
    EnsureDirectory(ROOT_PATH);
    EnsureDirectory(BIN_PATH);

    if (FileExists(MSCORLIB_PATH))
    {
        printf("[RUNTIME] mscorlib.pe found\n");
        return true;
    }

    printf("[RUNTIME] mscorlib.pe missing, downloading...\n");
    set_status_text("Restoring mscorlib.pe...", false);

    bool ok = DownloadFile(
        MSCORLIB_URL,
        MSCORLIB_PATH
    );

    if (!ok)
    {
        printf("[RUNTIME] release asset download failed, trying raw fallback...\n");

        ok = DownloadFile(
            MSCORLIB_FALLBACK_URL,
            MSCORLIB_PATH
        );
    }

    if (!ok)
    {
        printf("[RUNTIME] mscorlib.pe restore failed\n");
        set_status_text("mscorlib.pe restore failed!", true);
        return false;
    }

    if (!FileExists(MSCORLIB_PATH))
    {
        printf("[RUNTIME] mscorlib.pe downloaded but file not found\n");
        set_status_text("mscorlib.pe restore failed!", true);
        return false;
    }

    printf("[RUNTIME] mscorlib.pe restored\n");
    set_status_text("mscorlib.pe restored!", true);

    return true;
}



// ------------------------------------------------------------
// Filesystem helpers
// ------------------------------------------------------------

static bool path_is_directory(const char* path)
{
    struct stat st;

    if (stat(path, &st) != 0)
    {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

static bool str_equals_ignore_case(const char* a, const char* b)
{
    if (!a || !b)
    {
        return false;
    }

    while (*a && *b)
    {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);

        if (ca != cb)
        {
            return false;
        }

        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static bool ends_with_pe(const char* name)
{
    if (!name)
    {
        return false;
    }

    int len = strlen(name);

    if (len < 4)
    {
        return false;
    }

    const char* ext = name + len - 3;

    return str_equals_ignore_case(ext, ".pe");
}

static bool is_mscorlib_file(const char* name)
{
    return str_equals_ignore_case(name, "mscorlib.pe");
}

static bool copy_json_string_value(const char* json, const char* key, char* out, size_t outSize)
{
    if (!json || !key || !out || outSize == 0)
    {
        return false;
    }

    out[0] = '\0';

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p)
    {
        return false;
    }

    p = strchr(p, ':');
    if (!p)
    {
        return false;
    }

    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
        p++;
    }

    if (*p != '"')
    {
        return false;
    }

    p++;

    size_t index = 0;

    while (*p != '\0' && *p != '"' && index + 1 < outSize)
    {
        out[index++] = *p++;
    }

    out[index] = '\0';

    return index > 0;
}

static bool read_manifest_name_for_directory(const char* directoryPath, char* outName, size_t outNameSize)
{
    if (!directoryPath || !outName || outNameSize == 0)
    {
        return false;
    }

    outName[0] = '\0';

    char manifestPath[MAX_ITEM_PATH];
    snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", directoryPath);

    FILE* file = fopen(manifestPath, "rb");
    if (!file)
    {
        return false;
    }

    char json[512];
    size_t read = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);

    json[read] = '\0';

    return copy_json_string_value(json, "name", outName, outNameSize);
}

static int browser_item_rank(BrowserItemType type)
{
    if (type == ITEM_PARENT) return 0;
    if (type == ITEM_DIRECTORY) return 1;
    return 2;
}

static int compare_browser_items(const BrowserItem* a, const BrowserItem* b)
{
    int rankA = browser_item_rank(a->type);
    int rankB = browser_item_rank(b->type);

    if (rankA != rankB)
    {
        return rankA - rankB;
    }

    const char* pa = a->name;
    const char* pb = b->name;

    while (*pa && *pb)
    {
        char ca = (char)tolower((unsigned char)*pa);
        char cb = (char)tolower((unsigned char)*pb);

        if (ca != cb)
        {
            return (int)ca - (int)cb;
        }

        pa++;
        pb++;
    }

    return (int)(unsigned char)*pa - (int)(unsigned char)*pb;
}

static void sort_browser_items(void)
{
    for (int i = 0; i < browserItemCount - 1; i++)
    {
        for (int j = 0; j < browserItemCount - i - 1; j++)
        {
            if (compare_browser_items(&browserItems[j], &browserItems[j + 1]) > 0)
            {
                BrowserItem tmp = browserItems[j];
                browserItems[j] = browserItems[j + 1];
                browserItems[j + 1] = tmp;
            }
        }
    }
}

static bool is_root_directory(void)
{
    return strcmp(currentDir, ROOT_PATH) == 0;
}

static void reset_browser_selection(void)
{
    selectedIndex = 0;
    listOffset = 0;
    selectedAppPath[0] = '\0';
}

static void make_parent_path(const char* path, char* out, size_t outSize)
{
    if (!out || outSize == 0)
    {
        return;
    }

    out[0] = '\0';

    if (!path || path[0] == '\0')
    {
        snprintf(out, outSize, "%s", ROOT_PATH);
        return;
    }

    snprintf(out, outSize, "%s", path);

    int len = strlen(out);

    while (len > 0 && (out[len - 1] == '/' || out[len - 1] == '\\'))
    {
        out[len - 1] = '\0';
        len--;
    }

    int lastSlash = -1;

    for (int i = 0; out[i] != '\0'; i++)
    {
        if (out[i] == '/' || out[i] == '\\')
        {
            lastSlash = i;
        }
    }

    if (lastSlash >= 0)
    {
        out[lastSlash] = '\0';
    }

    if (strlen(out) < strlen(ROOT_PATH))
    {
        snprintf(out, outSize, "%s", ROOT_PATH);
    }

    if (strncmp(out, ROOT_PATH, strlen(ROOT_PATH)) != 0)
    {
        snprintf(out, outSize, "%s", ROOT_PATH);
    }

    if (out[0] == '\0')
    {
        snprintf(out, outSize, "%s", ROOT_PATH);
    }
}

static void set_current_directory(const char* path)
{
    if (!path || path[0] == '\0')
    {
        snprintf(currentDir, sizeof(currentDir), "%s", ROOT_PATH);
        return;
    }

    if (strncmp(path, ROOT_PATH, strlen(ROOT_PATH)) != 0)
    {
        snprintf(currentDir, sizeof(currentDir), "%s", ROOT_PATH);
        return;
    }

    snprintf(currentDir, sizeof(currentDir), "%s", path);
}


static const char* get_basename(const char* path)
{
    if (!path)
    {
        return "";
    }

    const char* last = path;

    for (const char* p = path; *p != '\0'; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
    }

    return last;
}

static bool read_manifest_for_pe(
    const char* directoryPath,
    const char* peName,
    char* displayName,
    size_t displayNameSize,
    char* author,
    size_t authorSize,
    char* version,
    size_t versionSize)
{
    if (!directoryPath || !peName || !displayName || displayNameSize == 0)
    {
        return false;
    }

    displayName[0] = '\0';

    if (author && authorSize > 0)
    {
        author[0] = '\0';
    }

    if (version && versionSize > 0)
    {
        version[0] = '\0';
    }

    char manifestPath[MAX_ITEM_PATH];

    int written = snprintf(
        manifestPath,
        sizeof(manifestPath),
        "%s/manifest.json",
        directoryPath
    );

    if (written <= 0 || written >= (int)sizeof(manifestPath))
    {
        return false;
    }

    FILE* f = fopen(manifestPath, "rb");

    if (!f)
    {
        return false;
    }

    char json[4096];
    size_t read = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);

    json[read] = '\0';

    char entry[MAX_ITEM_NAME];

    if (!copy_json_string_value(json, "entry", entry, sizeof(entry)))
    {
        return false;
    }

    const char* entryBase = get_basename(entry);

    if (!str_equals_ignore_case(entryBase, peName))
    {
        return false;
    }

    if (!copy_json_string_value(json, "name", displayName, displayNameSize))
    {
        snprintf(displayName, displayNameSize, "%s", peName);
    }

    if (author && authorSize > 0)
    {
        copy_json_string_value(json, "author", author, authorSize);
    }

    if (version && versionSize > 0)
    {
        copy_json_string_value(json, "version", version, versionSize);
    }

    return true;
}


static void add_browser_item(const char* name, const char* path, BrowserItemType type)
{
    if (browserItemCount >= MAX_BROWSER_ITEMS)
    {
        return;
    }

    snprintf(browserItems[browserItemCount].name, MAX_ITEM_NAME, "%s", name);
    snprintf(browserItems[browserItemCount].displayName, MAX_ITEM_NAME, "%s", name);
    browserItems[browserItemCount].author[0] = '\0';
    browserItems[browserItemCount].version[0] = '\0';
    snprintf(browserItems[browserItemCount].path, MAX_ITEM_PATH, "%s", path);
    browserItems[browserItemCount].type = type;

    browserItemCount++;
}

static void add_browser_app(const char* peName, const char* pePath, const char* directoryPath)
{
    if (browserItemCount >= MAX_BROWSER_ITEMS)
    {
        return;
    }

    char displayName[MAX_ITEM_NAME];
    char author[64];
    char version[32];

    snprintf(displayName, sizeof(displayName), "%s", peName);
    author[0] = '\0';
    version[0] = '\0';

    read_manifest_for_pe(
        directoryPath,
        peName,
        displayName,
        sizeof(displayName),
        author,
        sizeof(author),
        version,
        sizeof(version)
    );

    snprintf(browserItems[browserItemCount].name, MAX_ITEM_NAME, "%s", peName);
    snprintf(browserItems[browserItemCount].displayName, MAX_ITEM_NAME, "%s", displayName);
    snprintf(browserItems[browserItemCount].author, sizeof(browserItems[browserItemCount].author), "%s", author);
    snprintf(browserItems[browserItemCount].version, sizeof(browserItems[browserItemCount].version), "%s", version);
    snprintf(browserItems[browserItemCount].path, MAX_ITEM_PATH, "%s", pePath);
    browserItems[browserItemCount].type = ITEM_APP;

    browserItemCount++;
}

static void scan_current_directory(void)
{
    browserItemCount = 0;
    reset_browser_selection();

    if (!is_root_directory())
    {
        char parentPath[MAX_ITEM_PATH];
        make_parent_path(currentDir, parentPath, sizeof(parentPath));
        add_browser_item("..", parentPath, ITEM_PARENT);
    }

    DIR* dir = opendir(currentDir);

    if (!dir)
    {
        if(GetSystemLanguage() == 2)
        { // if console language is french
            set_status_text("Impossible d'ouvrir le dossier", true);
        } else
        {
            set_status_text("Cannot open folder", true);
        }
        return;
    }

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (browserItemCount >= MAX_BROWSER_ITEMS)
        {
            break;
        }

        const char* name = entry->d_name;

        if (!name || name[0] == '\0')
        {
            continue;
        }

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        {
            continue;
        }

        char fullPath[MAX_ITEM_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", currentDir, name);

        if (path_is_directory(fullPath))
        {
            char displayName[MAX_ITEM_NAME];

            if (read_manifest_name_for_directory(fullPath, displayName, sizeof(displayName)))
            {
                add_browser_item(displayName, fullPath, ITEM_DIRECTORY);
            }
            else
            {
                add_browser_item(name, fullPath, ITEM_DIRECTORY);
            }

            continue;
        }

        if (!ends_with_pe(name))
        {
            continue;
        }

        if (is_mscorlib_file(name))
        {
            continue;
        }

        add_browser_app(name, fullPath, currentDir);
    }

    closedir(dir);

    sort_browser_items();

    if (browserItemCount > 0)
    {
        if (browserItems[0].type == ITEM_APP)
        {
            snprintf(selectedAppPath, MAX_ITEM_PATH, "%s", browserItems[0].path);
        }
        else
        {
            selectedAppPath[0] = '\0';
        }
    }
}

static bool selected_item_is_app(void)
{
    if (selectedIndex < 0 || selectedIndex >= browserItemCount)
    {
        return false;
    }

    return browserItems[selectedIndex].type == ITEM_APP;
}

static bool selected_item_is_directory_like(void)
{
    if (selectedIndex < 0 || selectedIndex >= browserItemCount)
    {
        return false;
    }

    return browserItems[selectedIndex].type == ITEM_DIRECTORY ||
        browserItems[selectedIndex].type == ITEM_PARENT;
}

static void update_selected_app_path(void)
{
    selectedAppPath[0] = '\0';

    if (selected_item_is_app())
    {
        snprintf(selectedAppPath, MAX_ITEM_PATH, "%s", browserItems[selectedIndex].path);
    }
}

static void open_selected_directory(void)
{
    if (!selected_item_is_directory_like())
    {
        return;
    }

    set_current_directory(browserItems[selectedIndex].path);
    scan_current_directory();
}

static void check_filesystem(void)
{
    if (!EnsureDirectory(ROOT_PATH))
    {
        folderFound = false;
        mscorlibFound = false;
        // latestFound = false;
        canLaunch = false;
        browserItemCount = 0;
        selectedAppPath[0] = '\0';

        snprintf(
            statusText,
            sizeof(statusText),
            "Error creating folder. errno=%d",
            errno
        );

        return;
    }

    folderFound = true;

    if (currentDir[0] == '\0')
    {
        set_current_directory(ROOT_PATH);
    }

    mscorlibFound = ensure_runtime_files();
    // latestFound = ensure_latest_manifest_file();

    scan_current_directory();

    canLaunch = mscorlibFound && selected_item_is_app();

    if (!mscorlibFound)
    {
    
    if (GetSystemLanguage() == 2)
        { // if console language is french
            set_status_text("mscorlib.pe manquant!", false);
        } else
        {
            set_status_text("Missing mscorlib.pe!", false);
        }
    }
    else if (browserItemCount <= 0)
    {
        if (GetSystemLanguage() == 2)
        { // if console language is french
            set_status_text("Aucune app ou dossier trouvé", false);
        } else
        {
            set_status_text("No apps or folders found", false);
        }
    }
}

// ------------------------------------------------------------
// Drawing helpers
// ------------------------------------------------------------

static void draw_text(const char* text, float x, float y, float scale, u32 color)
{
    C2D_Text c2dText;

    C2D_TextParse(&c2dText, textBuf, text);
    C2D_TextOptimize(&c2dText);

    C2D_DrawText(
        &c2dText,
        C2D_WithColor,
        x,
        y,
        0.0f,
        scale,
        scale,
        color
    );
}

static void draw_text_centered(const char* text, float screenWidth, float y, float scale, u32 color)
{
    C2D_Text c2dText;
    float width = 0.0f;
    float height = 0.0f;

    C2D_TextParse(&c2dText, textBuf, text);
    C2D_TextOptimize(&c2dText);
    C2D_TextGetDimensions(&c2dText, scale, scale, &width, &height);

    C2D_DrawText(
        &c2dText,
        C2D_WithColor,
        (screenWidth - width) / 2.0f,
        y,
        0.0f,
        scale,
        scale,
        color
    );
}

static void draw_status_line(const char* label, bool ok, float y)
{
    u32 labelColor = C2D_Color32(190, 190, 205, 255);
    u32 okColor = ok
        ? C2D_Color32(90, 230, 130, 255)
        : C2D_Color32(240, 90, 90, 255);

    draw_text(label, 30, y, 0.45f, labelColor);
    if (GetSystemLanguage() == 2)
    { // if console language is french
        draw_text(ok ? "OK" : "MANQUANT", 310, y, 0.45f, okColor);
    }
    else
    {
        draw_text(ok ? "OK" : "MISSING", 310, y, 0.45f, okColor);
    }
}

static void make_short_path(const char* path, char* out, size_t outSize)
{
    if (!out || outSize == 0)
    {
        return;
    }

    if (!path)
    {
        out[0] = '\0';
        return;
    }

    const char* prefix = "sdmc:/ReSharp3DS";

    if (strncmp(path, prefix, strlen(prefix)) == 0)
    {
        const char* tail = path + strlen(prefix);

        if (tail[0] == '\0')
        {
            snprintf(out, outSize, "/");
        }
        else
        {
            snprintf(out, outSize, "%s", tail);
        }

        return;
    }

    snprintf(out, outSize, "%s", path);
}

static void draw_browser_list(void)
{
    u32 white = C2D_Color32(255, 255, 255, 255);
    u32 muted = C2D_Color32(175, 175, 195, 255);
    u32 selectedBg = C2D_Color32(92, 54, 190, 255);
    u32 rowBg = C2D_Color32(28, 28, 40, 255);
    u32 disabled = C2D_Color32(120, 120, 135, 255);
    u32 green = C2D_Color32(90, 230, 130, 255);
    u32 blue = C2D_Color32(95, 210, 255, 255);
    u32 yellow = C2D_Color32(245, 190, 90, 255);

    char shortPath[MAX_ITEM_PATH];
    make_short_path(currentDir, shortPath, sizeof(shortPath));

    draw_text(shortPath, 18, 31, 0.34f, muted);

    if (browserItemCount <= 0)
    {
        if (GetSystemLanguage() == 2)
        { // if console language is french
            draw_text_centered("Aucun dossier ou .pe trouvé", 320, 92, 0.52f, disabled);
            draw_text_centered("Mettez les apps dans sdmc:/ReSharp3DS", 320, 120, 0.42f, muted);
        } else
        {
            draw_text_centered("No folder or .pe found", 320, 92, 0.52f, disabled);
            draw_text_centered("Put apps in sdmc:/ReSharp3DS", 320, 120, 0.42f, muted);
        }

        return;
    }

    int visibleRows = 5;
    int rowHeight = 28;
    int startY = 55;

    if (selectedIndex < listOffset)
    {
        listOffset = selectedIndex;
    }

    if (selectedIndex >= listOffset + visibleRows)
    {
        listOffset = selectedIndex - visibleRows + 1;
    }

    for (int i = 0; i < visibleRows; i++)
    {
        int index = listOffset + i;

        if (index >= browserItemCount)
        {
            break;
        }

        int y = startY + i * rowHeight;
        bool selected = index == selectedIndex;

        C2D_DrawRectSolid(
            16,
            y,
            0,
            288,
            23,
            selected ? selectedBg : rowBg
        );

        char line[180];
        u32 textColor = muted;

        if (browserItems[index].type == ITEM_PARENT)
        {
            textColor = yellow;
        }
        else if (browserItems[index].type == ITEM_DIRECTORY)
        {
            textColor = blue;
        }
        else if (browserItems[index].type == ITEM_APP)
        {
            textColor = green;
        }

        const char* label = browserItems[index].displayName[0] != '\0'
            ? browserItems[index].displayName
            : browserItems[index].name;

        if (selected)
        {
            snprintf(line, sizeof(line), "> %s", label);
        }
        else
        {
            snprintf(line, sizeof(line), "  %s", label);
        }

        draw_text(line, 24, y + 4, 0.42f, selected ? white : textColor);
    }

    if (selectedIndex >= 0 &&
        selectedIndex < browserItemCount &&
        browserItems[selectedIndex].type == ITEM_APP)
    {
        char detailsLine[220];
        detailsLine[0] = '\0';

        if (browserItems[selectedIndex].author[0] != '\0' &&
            browserItems[selectedIndex].version[0] != '\0')
        {
            snprintf(
                detailsLine,
                sizeof(detailsLine),
                "Author: %s  Version: %s",
                browserItems[selectedIndex].author,
                browserItems[selectedIndex].version
            );
        }
        else if (browserItems[selectedIndex].author[0] != '\0')
        {
            snprintf(
                detailsLine,
                sizeof(detailsLine),
                "Author: %s",
                browserItems[selectedIndex].author
            );
        }
        else if (browserItems[selectedIndex].version[0] != '\0')
        {
            snprintf(
                detailsLine,
                sizeof(detailsLine),
                "Version: %s",
                browserItems[selectedIndex].version
            );
        }

        if (detailsLine[0] != '\0')
        {
            draw_text(detailsLine, 20, 198, 0.34f, muted);
        }
    }

    if (GetSystemLanguage() == 2)
    { // if console language is french
        draw_text("X: rafraichir", 20, 220, 0.38f, muted);
    } else
    {
        draw_text("X: refresh", 20, 220, 0.38f, muted);
    }

}

// ------------------------------------------------------------
// Init / render
// ------------------------------------------------------------

static void init_graphics_once(void)
{
    if (initialized)
    {
        return;
    }

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    cfguInit();

    topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    textBuf = C2D_TextBufNew(4096);

    set_current_directory(ROOT_PATH);
    check_filesystem();

    initialized = true;
}

void run_gui(void)
{
    init_graphics_once();
    update_status_text_timer();

    C2D_TextBufClear(textBuf);

    u32 bgTop = C2D_Color32(14, 14, 22, 255);
    u32 bgBottom = C2D_Color32(10, 10, 16, 255);
    u32 headerColor = C2D_Color32(92, 54, 190, 255);
    u32 cardColor = C2D_Color32(28, 28, 40, 255);
    u32 white = C2D_Color32(255, 255, 255, 255);
    u32 cyan = C2D_Color32(95, 210, 255, 255);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    // Top screen
    C2D_TargetClear(topScreen, bgTop);
    C2D_SceneBegin(topScreen);

    C2D_DrawRectSolid(0, 0, 0, 400, 42, headerColor);

    draw_text("ReSharp3DS Runtime", 18, 9, 0.62f, white);
    draw_text(APP_VERSION, 300, 12, 0.45f, C2D_Color32(230, 220, 255, 255));

    const char* message = " ";

if(GetSystemLanguage() == 2)
        { // if console language is french
            message = "Supporter le Projet - github.com/saysaa/ReSharp3DS";
        }
        else
        {
            message = "Support project : github.com/saysaa/ReSharp3DS";
        }
    draw_text_centered(
        message,
        400,
        51,
        0.45f,
        cyan
    );

    C2D_DrawRectSolid(18, 78, 0, 364, 112, cardColor);
    C2D_DrawRectSolid(18, 78, 0, 4, 112, headerColor);

    if (GetSystemLanguage() == 2)
    { // if console language is french
        draw_text("Verification de l'integrité", 32, 90, 0.55f, white);
    } else
    {
        draw_text("Integrity Check", 32, 90, 0.55f, white);
    }

    if(GetSystemLanguage() == 2)
    { // if console language is french
        draw_status_line("Dossier - :sdmc/ReSharp3DS", folderFound, 120);
        draw_status_line("mscorlib.pe", mscorlibFound, 143);
        draw_status_line("Explorateur", browserItemCount > 0, 166);
    } else
    {
        draw_status_line("Folder - :sdmc/ReSharp3DS", folderFound, 120);
        draw_status_line("mscorlib.pe", mscorlibFound, 143);
        draw_status_line("Explorer", browserItemCount > 0, 166);
    }


    draw_text_centered(
        statusText,
        400,
        207,
        0.43f,
        canLaunch
        ? C2D_Color32(100, 240, 140, 255)
        : C2D_Color32(245, 190, 90, 255)
    );

    // Bottom screen
    C2D_TargetClear(bottomScreen, bgBottom);
    C2D_SceneBegin(bottomScreen);

    draw_browser_list();

    C3D_FrameEnd(0);

}

// ------------------------------------------------------------
// Public GUI API
// ------------------------------------------------------------

bool gui_handle_touch(int x, int y)
{
    init_graphics_once();

    if (browserItemCount <= 0)
    {
        return false;
    }

    // Touch scroll shortcuts on the right edge.
    if (x >= 292 && x <= 319 && y >= 55 && y < 115)
    {
        gui_move_selection(-1);
        return false;
    }

    if (x >= 292 && x <= 319 && y >= 115 && y < 195)
    {
        gui_move_selection(1);
        return false;
    }

    // Browser rows on the bottom screen.
    const int listX = 16;
    const int listY = 55;
    const int listWidth = 288;
    const int rowHeight = 28;
    const int visibleRows = 5;

    if (x >= listX && x < listX + listWidth &&
        y >= listY && y < listY + rowHeight * visibleRows)
    {
        int row = (y - listY) / rowHeight;
        int index = listOffset + row;

        if (index < 0 || index >= browserItemCount)
        {
            return false;
        }

        selectedIndex = index;
        update_selected_app_path();
        canLaunch = mscorlibFound && selected_item_is_app();

        // Let main.cpp call gui_can_launch().
        // If the selected item is a folder, gui_can_launch() will open it.
        // If it is a .pe app, gui_can_launch() will allow launch.
        return true;
    }

    // Small touch shortcut over the bottom help line.
    // This matches the "X: refresh" action.
    if (x >= 12 && x <= 130 && y >= 210 && y <= 239)
    {
        gui_refresh_files();
        return false;
    }

    return false;
}

void gui_move_selection(int direction)
{
    if (browserItemCount <= 0)
    {
        return;
    }

    selectedIndex += direction;

    if (selectedIndex < 0)
    {
        selectedIndex = browserItemCount - 1;
    }

    if (selectedIndex >= browserItemCount)
    {
        selectedIndex = 0;
    }

    update_selected_app_path();

    canLaunch = mscorlibFound && selected_item_is_app();
}

void gui_refresh_files(void)
{
    check_filesystem();
}

bool gui_can_launch(void)
{
    init_graphics_once();

    if (!mscorlibFound)
    {
        canLaunch = false;
        if (GetSystemLanguage() == 2)
        { // if console language is french
            set_status_text("mscorlib.pe manquant!", false);
        } else
        {
            set_status_text("Missing mscorlib.pe!", false);
        }
        return false;
    }

    if (browserItemCount <= 0)
    {
        canLaunch = false;
        if (GetSystemLanguage() == 2)
        { // if console language is french
            set_status_text("Aucun dossier ou .pe trouvé", false);
        } else
        {
            set_status_text("No apps or folders found", false);
        }
        return false;
    }

    if (selected_item_is_directory_like())
    {
        open_selected_directory();
        canLaunch = false;
        return false;
    }

    if (selected_item_is_app())
    {
        update_selected_app_path();
        canLaunch = true;
        return true;
    }

    canLaunch = false;
    return false;
}

const char* gui_get_selected_app_path(void)
{
    if (!selected_item_is_app())
    {
        return NULL;
    }

    return selectedAppPath;
}

void gui_shutdown(void)
{
    if (!initialized)
    {
        return;
    }

    if (textBuf)
    {
        C2D_TextBufDelete(textBuf);
        textBuf = nullptr;
    }

    C2D_Fini();
    C3D_Fini();

    cfguExit();

    topScreen = nullptr;
    bottomScreen = nullptr;

    initialized = false;
}
