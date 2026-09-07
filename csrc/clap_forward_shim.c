/* clap_forward_shim.c -- the Windows .clap of a Julia step: a DLL with no
 * Julia in it, which loads the real plugin from beside itself and forwards
 * clap_entry to it.
 *
 * Why. A juliac-built plugin statically imports libjulia.dll and the rest
 * of the runtime. Windows has no rpath: the loader resolves a DLL's static
 * imports from the host executable's directory, the system directories,
 * the current directory and PATH -- and from the directory of the DLL
 * being loaded only when that DLL was opened with
 * LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH). A DAW opens a .clap
 * with plain LoadLibrary, so a runtime placed next to the plugin is never
 * searched and the load fails before clap_entry is reached.
 *
 * So the .clap is this shim, which imports nothing but kernel32. Its
 * init(path) takes the path the host passes (the shim's own), computes
 *
 *     <path>.runtime\bin\<AP_INNER_BASE>.dll
 *
 * and opens it with LOAD_WITH_ALTERED_SEARCH_PATH, so the runtime DLLs
 * beside the inner plugin resolve. Before that it puts the same directory
 * at the front of the process PATH: libjulia opens libpcre2-8, libgmp and
 * friends by bare name at runtime initialisation, and a bare name follows
 * the standard search order, in which nothing points at the bundle. The
 * PATH entry is what makes them found (a DAW's own directory is searched
 * first, so a DAW that ships those libraries keeps its own).
 * get_factory and deinit forward to the inner entry. The inner DLL is
 * exactly the plugin that is the .clap on Linux and macOS.
 *
 * AP_INNER_BASE is the inner DLL's file name without .dll, a C identifier
 * given on the command line (-DAP_INNER_BASE=libjl_eq); nothing about the
 * build machine is baked in.
 */

#ifndef _WIN32
#error "clap_forward_shim.c is the Windows loader shim; other platforms use an rpath"
#endif

#ifndef AP_INNER_BASE
#error "define AP_INNER_BASE to the inner DLL's file name without .dll"
#endif

#include "clap/clap.h"

#include <windows.h>
#include <string.h>

#define AP_WSTR_(x) L ## #x
#define AP_WSTR(x) AP_WSTR_(x)
#define INNER_SUFFIX L".runtime\\bin\\" AP_WSTR(AP_INNER_BASE) L".dll"

static HMODULE                   INNER;
static const clap_plugin_entry_t *INNER_ENTRY;
static int                        INITED;    /* init() calls minus deinit() calls */

static bool to_wide(const char *s, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n <= 0)         /* not valid UTF-8: a host using the ANSI code page */
        n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap);
    return n > 0;
}

static bool shim_init(const char *path) {
    if (INITED > 0) { INITED++; return true; }
    if (!path) return false;

    wchar_t given[4096], full[4096], inner[4096];
    char inner_utf8[4096 * 3];
    if (!to_wide(path, given, 4096)) return false;
    DWORD n = GetFullPathNameW(given, 4096, full, NULL);
    if (n == 0 || n >= 4096) return false;
    if (n + wcslen(INNER_SUFFIX) >= 4096) return false;
    wcscpy(inner, full);
    wcscat(inner, INNER_SUFFIX);

    /* <path>.runtime\bin goes to the front of PATH, see the header comment. */
    wchar_t bindir[4096];
    wcscpy(bindir, inner);
    wchar_t *sep = wcsrchr(bindir, L'\\');
    if (sep) *sep = L'\0';
    DWORD plen = GetEnvironmentVariableW(L"PATH", NULL, 0);
    wchar_t *newpath = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                            (wcslen(bindir) + 1 + plen + 1) * sizeof(wchar_t));
    if (newpath) {
        wcscpy(newpath, bindir);
        if (plen > 0) {
            wcscat(newpath, L";");
            GetEnvironmentVariableW(L"PATH", newpath + wcslen(newpath), plen);
        }
        SetEnvironmentVariableW(L"PATH", newpath);
        HeapFree(GetProcessHeap(), 0, newpath);
    }

    INNER = LoadLibraryExW(inner, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!INNER) return false;
    INNER_ENTRY = (const clap_plugin_entry_t *)(void *)GetProcAddress(INNER, "clap_entry");
    if (!INNER_ENTRY || !WideCharToMultiByte(CP_UTF8, 0, inner, -1, inner_utf8,
                                             sizeof inner_utf8, NULL, NULL) ||
        (INNER_ENTRY->init && !INNER_ENTRY->init(inner_utf8))) {
        FreeLibrary(INNER);
        INNER = NULL;
        INNER_ENTRY = NULL;
        return false;
    }
    INITED = 1;
    return true;
}

static void shim_deinit(void) {
    if (INITED <= 0 || --INITED > 0) return;
    if (INNER_ENTRY && INNER_ENTRY->deinit) INNER_ENTRY->deinit();
    /* The inner DLL stays loaded: a Julia runtime cannot be torn down and
     * started again in one process, and a DAW may init the entry again. */
    INNER_ENTRY = NULL;
}

static const void *shim_get_factory(const char *id) {
    return (INITED > 0 && INNER_ENTRY && INNER_ENTRY->get_factory) ? INNER_ENTRY->get_factory(id) : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = shim_init,
    .deinit = shim_deinit,
    .get_factory = shim_get_factory,
};
