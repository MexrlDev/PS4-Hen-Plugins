/*
 * PurpyHen 3.0.0 - USB PKG Browser
 *
 * Same scanner logic as plugin_server/source/cmd.c so FTP and ShellUI agree
 * on what's on the drives.
 */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../common/plugin_common.h"
#include "../../common/notify.h"
#include "../../common/path.h"
#include "../../common/file.h"
#include "../../common/function_ptr.h"

#include "pkg_browser.h"

/* ====================================================================
 * Tunables
 * ==================================================================== */
#define PKG_MAX_RESULTS   1024
#define PKG_MAX_PATH_LEN  512
#define PKG_MAX_DEPTH     12
#define PKG_TID_LEN       16

/* ====================================================================
 * Types
 * ==================================================================== */
typedef struct
{
    char path[PKG_MAX_PATH_LEN];
    char title_id[PKG_TID_LEN];
    uint64_t size;
} pkg_entry_t;

typedef struct
{
    pkg_entry_t entries[PKG_MAX_RESULTS];
    int count;
    int overflow;
} pkg_list_t;

static pkg_list_t g_last_scan;
static int        g_has_scanned = 0;

/* ====================================================================
 * Utilities
 * ==================================================================== */

static bool has_pkg_ext(const char* name)
{
    size_t len = strlen(name);
    if (len < 5) return false;
    const char* ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'k' || ext[2] == 'K') &&
            (ext[3] == 'g' || ext[3] == 'G'));
}

/*
 * Extract a title ID from a PKG filename.
 * PS4 title IDs look like: CUSA00001, PCAS00001, PCSB00001, UCUS98765...
 * Pattern: [CPU][A-Z]{3}[0-9]{5}  (10 chars total), surrounded by non-alnum
 */
static void extract_title_id(const char* filename, char* out, size_t out_sz)
{
    out[0] = '\0';
    if (!filename || !out || out_sz < 2) return;

    const char* p = filename;
    while (*p)
    {
        /* candidate needs at least 10 chars and a leading C/P/U */
        if ((p[0] == 'C' || p[0] == 'P' || p[0] == 'U') &&
            p[1] && p[2] && p[3] && p[4] &&
            p[5] && p[6] && p[7] && p[8] && p[9])
        {
            bool ok = true;

            /* must not be preceded by an alnum */
            if (p != filename &&
                ((p[-1] >= 'A' && p[-1] <= 'Z') ||
                 (p[-1] >= '0' && p[-1] <= '9')))
                ok = false;

            if (ok)
            {
                for (int i = 0; i < 10; i++)
                {
                    if (!((p[i] >= 'A' && p[i] <= 'Z') ||
                          (p[i] >= '0' && p[i] <= '9')))
                    {
                        ok = false;
                        break;
                    }
                }
            }

            /* must not be followed by an alnum */
            if (ok && ((p[10] >= 'A' && p[10] <= 'Z') ||
                       (p[10] >= '0' && p[10] <= '9')))
                ok = false;

            if (ok)
            {
                size_t copy = 10;
                if (copy >= out_sz) copy = out_sz - 1;
                memcpy(out, p, copy);
                out[copy] = '\0';
                return;
            }
        }
        p++;
    }
}

static void format_size(uint64_t bytes, char* out, size_t out_sz)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        snprintf(out, out_sz, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= (uint64_t)1024 * 1024)
        snprintf(out, out_sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, out_sz, "%llu KB", (unsigned long long)(bytes / 1024));
}

/*
 * XML-escape a string. Writes at most out_sz - 1 chars plus null.
 * Handles & < > " '
 */
static void xml_escape(const char* in, char* out, size_t out_sz)
{
    size_t o = 0;
    if (!in || !out || out_sz == 0) { if (out_sz) out[0] = 0; return; }

    for (const char* p = in; *p && o + 6 < out_sz; p++)
    {
        switch (*p)
        {
        case '&':  memcpy(out + o, "&amp;",  5); o += 5; break;
        case '<':  memcpy(out + o, "&lt;",   4); o += 4; break;
        case '>':  memcpy(out + o, "&gt;",   4); o += 4; break;
        case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(out + o, "&apos;", 6); o += 6; break;
        default:   out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}

static void mkdir_p(const char* path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ====================================================================
 * Scanner
 * ==================================================================== */

static void scan_dir_recursive(const char* dir, pkg_list_t* list, int depth)
{
    if (!dir || !list || depth > PKG_MAX_DEPTH) return;
    if (list->count >= PKG_MAX_RESULTS) { list->overflow = 1; return; }

    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (!strcmp(ent->d_name, ".git") ||
            !strcmp(ent->d_name, "node_modules")) continue;

        char full[PKG_MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode))
        {
            scan_dir_recursive(full, list, depth + 1);
        }
        else if (S_ISREG(st.st_mode) && has_pkg_ext(ent->d_name))
        {
            if (list->count >= PKG_MAX_RESULTS) { list->overflow = 1; break; }

            pkg_entry_t* e = &list->entries[list->count++];
            strncpy(e->path, full, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            e->size = (uint64_t)st.st_size;
            extract_title_id(ent->d_name, e->title_id, sizeof(e->title_id));
        }
    }
    closedir(d);
}

static void scan_all_usb(pkg_list_t* list)
{
    memset(list, 0, sizeof(*list));

    for (int i = 0; i <= 7; i++)
    {
        char root[32];
        struct stat st;

        snprintf(root, sizeof(root), "/mnt/usb%d", i);
        if (stat(root, &st) == 0 && S_ISDIR(st.st_mode))
            scan_dir_recursive(root, list, 0);

        /* Alt naming: /mnt/usb0.0, /mnt/usb0.1 ... */
        for (int j = 0; j <= 7; j++)
        {
            char alt[48];
            snprintf(alt, sizeof(alt), "/mnt/usb%d.%d", i, j);
            if (stat(alt, &st) == 0 && S_ISDIR(st.st_mode))
                scan_dir_recursive(alt, list, 0);
        }
    }
}

/* ====================================================================
 * XML page generation
 * ==================================================================== */

#define PKG_PAGE_DIR  SHELLUI_DATA_PATH "/PkgInstaller/data"
#define PKG_PAGE_PATH PKG_PAGE_DIR "/pkginstaller_all_usb.xml"

static int write_page(const pkg_list_t* list)
{
    mkdir_p(SHELLUI_DATA_PATH);
    mkdir_p(SHELLUI_DATA_PATH "/PkgInstaller");
    mkdir_p(PKG_PAGE_DIR);

    FILE* f = fopen(PKG_PAGE_PATH, "wb");
    if (!f)
    {
        final_printf("PkgBrowser: failed to open %s for writing\n", PKG_PAGE_PATH);
        return -1;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n");
    fprintf(f, "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n");
    fprintf(f, "  <setting_list id=\"pkginstaller_all_usb\" title=\"\xE2\x98\x85 USB PKGs Found (%d)\">\n",
            list->count);

    if (list->count == 0)
    {
        fprintf(f, "    <button id=\"pkg_none\" title=\"No PKGs found\" "
                   "description=\"Insert a USB drive with .pkg files and reopen this page.\"/>\n");
    }
    else
    {
        for (int i = 0; i < list->count; i++)
        {
            const pkg_entry_t* e = &list->entries[i];

            char title[64];
            if (e->title_id[0])
                snprintf(title, sizeof(title), "[%s]", e->title_id);
            else
                snprintf(title, sizeof(title), "PKG #%d", i + 1);

            char size_str[24];
            format_size(e->size, size_str, sizeof(size_str));

            char path_esc[PKG_MAX_PATH_LEN * 6];
            char title_esc[64 * 6];
            xml_escape(e->path, path_esc, sizeof(path_esc));
            xml_escape(title,    title_esc, sizeof(title_esc));

            /* description with line break (&#xa; is literal newline in XML) */
            char desc[PKG_MAX_PATH_LEN * 6 + 128];
            snprintf(desc, sizeof(desc),
                     "Install package from %s&#xa;Size: %s",
                     path_esc, size_str);

            /* The id includes the index so ShellUI can map clicks back to us.
             * The file attribute uses the pkg: URI scheme as a hint; if your
             * ShellUI doesn't understand pkg:, the button will still render
             * and the FTP SITE PKGS command remains a fallback. */
            fprintf(f,
                    "    <button id=\"pkg_%d\" title=\"%s\" description=\"%s\" "
                    "file=\"pkg:%s\"/>\n",
                    i, title_esc, desc, path_esc);
        }

        if (list->overflow)
        {
            fprintf(f, "    <button id=\"pkg_overflow\" title=\"List truncated\" "
                       "description=\"More than %d PKGs found; only the first %d are shown.\"/>\n",
                    PKG_MAX_RESULTS, PKG_MAX_RESULTS);
        }
    }

    fprintf(f, "  </setting_list>\n");
    fprintf(f, "</system_settings>\n");
    fclose(f);

    final_printf("PkgBrowser: wrote %s (%d entries)\n", PKG_PAGE_PATH, list->count);
    return 0;
}

/* ====================================================================
 * Installer
 * ==================================================================== */

/* sceAppInstUtilAppInstallPkg - resolve dynamically so we don't need the
 * full SDK header at build time, and so we degrade gracefully if the FW
 * doesn't expose it. */
typedef int (*install_pkg_fn)(const char* uri,
                              const char* playgo_scenario_id,
                              const char* label,
                              const char* entitlement_key);

static install_pkg_fn resolve_installer(void)
{
    /* 0x2001 is the standard libkernel handle on PS4. The AppInstUtil
     * symbol is exported by libSceAppInstUtil which loads into ShellUI. */
    static install_pkg_fn cached = NULL;
    static int tried = 0;
    if (tried) return cached;
    tried = 1;

    void* sym = NULL;

    /* Try 0x2001 first (common for libkernel). If your SDK exposes
     * sceKernelDlsym, this will work as-is. */
    extern int sceKernelDlsym(int handle, const char* symbol, void** addr);
    if (sceKernelDlsym(0x2001, "sceAppInstUtilAppInstallPkg", &sym) != 0 || !sym)
    {
        /* Fallback: search the currently loaded modules. */
        sym = NULL;
    }

    if (!sym)
    {
        /* Second chance: some firmware exposes the symbol from module 2. */
        if (sceKernelDlsym(2, "sceAppInstUtilAppInstallPkg", &sym) != 0)
            sym = NULL;
    }

    cached = (install_pkg_fn)sym;
    final_printf("PkgBrowser: installer resolved to 0x%p\n", (void*)cached);
    return cached;
}

int PkgBrowser_InstallByPath(const char* pkg_path)
{
    if (!pkg_path || !pkg_path[0])
        return -1;

    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        Notify("", "PKG not found:\n%s", pkg_path);
        return -2;
    }

    install_pkg_fn fn = resolve_installer();
    if (!fn)
    {
        Notify("",
               "Cannot resolve sceAppInstUtilAppInstallPkg.\n"
               "Use FTP: SITE PKGS then RETR the file.");
        return -3;
    }

    int rc = fn(pkg_path, NULL, NULL, NULL);
    final_printf("PkgBrowser: install('%s') -> 0x%08x\n", pkg_path, rc);

    if (rc == 0)
        Notify("", "Installing:\n%s", pkg_path);
    else
        Notify("", "Install failed 0x%08x\n%s", rc, pkg_path);

    return rc;
}

/* ====================================================================
 * Public entry points
 * ==================================================================== */

int PkgBrowser_GetLastCount(void)
{
    return g_has_scanned ? g_last_scan.count : 0;
}

void PkgBrowser_Refresh(void)
{
    final_printf("PkgBrowser: scanning USB drives...\n");
    scan_all_usb(&g_last_scan);
    g_has_scanned = 1;
    final_printf("PkgBrowser: found %d PKG(s)\n", g_last_scan.count);
    write_page(&g_last_scan);
}

void PkgBrowser_Init(void)
{
    /* Make sure the base dirs exist even if no USB is plugged in. */
    mkdir_p(SHELLUI_DATA_PATH);
    mkdir_p(SHELLUI_DATA_PATH "/PkgInstaller");
    mkdir_p(PKG_PAGE_DIR);

    PkgBrowser_Refresh();
}
