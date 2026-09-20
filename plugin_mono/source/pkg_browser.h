#pragma once

/*
 * PurpyHen 3.0.0 - USB PKG Browser
 *
 * Scans /mnt/usb0 .. /mnt/usb7 (and .N sub-variants) recursively for .pkg
 * files, extracts title IDs from filenames, and generates a ShellUI settings
 * page that lists them all. Also exposes a direct install helper used by the
 * dynamic page and by any other component that wants to trigger an install.
 *
 * The page XML is written to:
 *   /data/hen/shellui_data/PkgInstaller/data/pkginstaller_all_usb.xml
 * and is referenced from hen_settings.xml via the "PKG Browser (All USB)"
 * link that was added in PurpyHen 3.0.0.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Scans USB drives, generates the XML page, writes it to disk.
 * Safe to call multiple times. Intended to be called from plugin_load
 * and also whenever the user might want a fresh list. */
void PkgBrowser_Init(void);

/* Rescan USB and regenerate the XML on demand. */
void PkgBrowser_Refresh(void);

/* Install a .pkg by absolute path. Returns 0 on success (submission),
 * negative on failure. Non-blocking: the console will show its normal
 * install notifications. */
int PkgBrowser_InstallByPath(const char* pkg_path);

/* Returns the number of PKGs found on the last scan. */
int PkgBrowser_GetLastCount(void);

#ifdef __cplusplus
}
#endif
