/* Copyright (C) 2023 John Törnblom
 * Copyright (C) 2026 PurpyHen contributors
 *
 * GPLv3 or later.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <sys/_iovec.h>
#endif
#include <sys/mount.h>

#include "cmd.h"
#include "log.h"

#ifdef __PROSPERO__
#define IOVEC_ENTRY(x)            \
    {                             \
        x ? x : 0,                \
            x ? strlen(x) + 1 : 0 \
    }
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#endif

struct tm* localtime_s(const time_t* t, struct tm* tm);
#define LOCALTIME_R(t, tm) localtime_s(t, tm)

/* ====================================================================
 * PKG scanning helpers (new in PurpyHen 3.0.0)
 * ==================================================================== */

#define PKG_MAX_RESULTS   1024
#define PKG_MAX_PATH_LEN  512
#define PKG_MAX_DEPTH     12

typedef struct
{
    char path[PKG_MAX_PATH_LEN];
    char title_id[16];
    uint64_t size;
} pkg_entry_t;

typedef struct
{
    pkg_entry_t entries[PKG_MAX_RESULTS];
    int count;
    int overflow;
} pkg_list_t;

/* Extract title ID from a PS4 PKG filename.
 * Typical shape:  REGION-PUBLISHER-TITLEID_XX-CONTENT.pkg
 * We look for a token of the form  [PCU][A-Z]{2}[0-9]{5}
 * (CUSA, PCAS, PCJS, PCKS, PCSB, PCSF, PCSG, PCSH, UCUS, UCES, etc.)
 */
static void extract_title_id(const char* filename, char* out, size_t out_sz)
{
    out[0] = '\0';
    if (!filename || !out || out_sz < 2)
        return;

    const char* p = filename;
    while (*p)
    {
        /* candidate start: one of P/C/U, then 9 alnum chars */
        if ((p[0] == 'C' || p[0] == 'P' || p[0] == 'U') &&
            p[1] && p[2] && p[3] && p[4] && p[5] && p[6] && p[7] && p[8] && p[9])
        {
            bool ok = true;
            /* must not be preceded by an alnum */
            if (p != filename && ((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9')))
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
            if (ok && ((p[10] >= 'A' && p[10] <= 'Z') || (p[10] >= '0' && p[10] <= '9')))
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

static void scan_dir_recursive(const char* dir, pkg_list_t* list, int depth)
{
    if (!dir || !list || depth > PKG_MAX_DEPTH)
        return;
    if (list->count >= PKG_MAX_RESULTS)
    {
        list->overflow = 1;
        return;
    }

    DIR* d = opendir(dir);
    if (!d)
        return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        /* skip obvious system dirs that would recurse forever */
        if (!strcmp(ent->d_name, ".git") ||
            !strcmp(ent->d_name, "node_modules"))
            continue;

        char full[PKG_MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            scan_dir_recursive(full, list, depth + 1);
        }
        else if (S_ISREG(st.st_mode) && has_pkg_ext(ent->d_name))
        {
            if (list->count >= PKG_MAX_RESULTS)
            {
                list->overflow = 1;
                break;
            }
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
        snprintf(root, sizeof(root), "/mnt/usb%d", i);
        struct stat st;
        if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        /* also check /mnt/usb0.N variants just in case */
        scan_dir_recursive(root, list, 0);

        /* alt naming: /mnt/usb0/0, sometimes m2 card is /mnt/usb1 */
        for (int j = 0; j <= 7; j++)
        {
            char alt[48];
            snprintf(alt, sizeof(alt), "/mnt/usb%d.%d", i, j);
            if (stat(alt, &st) == 0 && S_ISDIR(st.st_mode))
                scan_dir_recursive(alt, list, 0);
        }
    }
}

/* Build a "/pkgs" listing block (one pkg per line) */
static int ftp_send_pkg_listing(ftp_env_t* env)
{
    pkg_list_t list;
    scan_all_usb(&list);

    if (ftp_data_open(env))
        return ftp_perror(env);

    ftp_active_printf(env, "150 Opening data transfer\r\n");

    char timebuf[20] = "Jan 01 00:00";
    for (int i = 0; i < list.count; i++)
    {
        pkg_entry_t* e = &list.entries[i];
        const char* base = strrchr(e->path, '/');
        base = base ? base + 1 : e->path;

        if (env->show_title_id && e->title_id[0])
        {
            ftp_data_printf(env,
                "-rw-rw-rw- 1 0 0 %" PRIu64 " %s %s [TID:%s] %s\r\n",
                e->size, timebuf, base, e->title_id, e->path);
        }
        else
        {
            ftp_data_printf(env,
                "-rw-rw-rw- 1 0 0 %" PRIu64 " %s %s %s\r\n",
                e->size, timebuf, base, e->path);
        }
    }

    if (list.overflow)
        ftp_data_printf(env, "# WARNING: more than %d PKGs found, list truncated\r\n", PKG_MAX_RESULTS);

    if (ftp_data_close(env))
        return ftp_perror(env);

    return ftp_active_printf(env, "226 Transfer complete\r\n");
}

/* ====================================================================
 * Standard FTP commands
 * ==================================================================== */

static void ftp_mode_string(mode_t mode, char* buf)
{
    char c, d;
    int i, bit;

    buf[10] = 0;
    for (i = 0; i < 9; i++)
    {
        bit = mode & (1 << i);
        c = i % 3;
        if (!c && (mode & (1 << ((d = i / 3) + 9))))
        {
            c = "tss"[(int)d];
            if (!bit)
                c &= ~0x20;
        }
        else
        {
            c = bit ? "xwr"[(int)c] : '-';
        }
        buf[9 - i] = c;
    }

    if (S_ISDIR(mode)) c = 'd';
    else if (S_ISBLK(mode)) c = 'b';
    else if (S_ISCHR(mode)) c = 'c';
    else if (S_ISLNK(mode)) c = 'l';
    else if (S_ISFIFO(mode)) c = 'p';
    else if (S_ISSOCK(mode)) c = 's';
    else c = '-';
    *buf = c;
}

static int ftp_data_open(ftp_env_t* env)
{
    struct sockaddr_in data_addr;
    socklen_t addr_len;

    if (env->data_addr.sin_port)
    {
        if (connect(env->data_fd, (struct sockaddr*)&env->data_addr, sizeof(env->data_addr)))
            return -1;
    }
    else
    {
        if ((env->data_fd = accept(env->passive_fd, (struct sockaddr*)&data_addr, &addr_len)) < 0)
            return -1;
    }
    return 0;
}

static int ftp_data_printf(ftp_env_t* env, const char* fmt, ...)
{
    char buf[0x1000];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

#if LOG_PRINTF_TO_TTY
    printf("[ftpsrv] %s", buf);
#endif
    size_t len = strlen(buf);
    if (write(env->data_fd, buf, len) != len)
        return -1;
    return 0;
}

static int ftp_data_read(ftp_env_t* env, void* buf, size_t count)
{
    return recv(env->data_fd, buf, count, 0);
}

static int ftp_data_close(ftp_env_t* env)
{
    if (!close(env->data_fd))
        return 0;
    return -1;
}

static int ftp_active_printf(ftp_env_t* env, const char* fmt, ...)
{
    char buf[0x1000];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

#if LOG_PRINTF_TO_TTY
    printf("[ftpsrv] %s", buf);
#endif
    size_t len = strlen(buf);
    if (write(env->active_fd, buf, len) != len)
        return -1;
    return 0;
}

static int ftp_perror(ftp_env_t* env)
{
    char buf[255];
    if (strerror_r(errno, buf, sizeof(buf)))
        strncpy(buf, "Unknown error", sizeof(buf));
    return ftp_active_printf(env, "550 %s\r\n", buf);
}

static void ftp_abspath(ftp_env_t* env, char* abspath, const char* path)
{
    char buf[PATH_MAX + 1];
    if (path[0] != '/')
    {
        snprintf(buf, sizeof(buf), "%s/%s", env->cwd, path);
        strncpy(abspath, buf, PATH_MAX);
    }
    else
    {
        strncpy(abspath, path, PATH_MAX);
    }
}

/* ====================================================================
 * Command implementations
 * ==================================================================== */

int ftp_cmd_PASV(ftp_env_t* env, const char* arg)
{
    socklen_t sockaddr_len = sizeof(struct sockaddr_in);
    struct sockaddr_in sockaddr;
    uint32_t addr = 0;
    uint16_t port = 0;

    if (getsockname(env->active_fd, (struct sockaddr*)&sockaddr, &sockaddr_len))
        return ftp_perror(env);
    addr = sockaddr.sin_addr.s_addr;

    if (env->passive_fd > 0)
        close(env->passive_fd);

    if ((env->passive_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return ftp_perror(env);

    memset(&sockaddr, 0, sockaddr_len);
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockaddr.sin_port = htons(0);

    if (bind(env->passive_fd, (struct sockaddr*)&sockaddr, sockaddr_len) != 0)
    {
        int ret = ftp_perror(env);
        close(env->passive_fd);
        return ret;
    }
    if (listen(env->passive_fd, 5) != 0)
    {
        int ret = ftp_perror(env);
        close(env->passive_fd);
        return ret;
    }
    if (getsockname(env->passive_fd, (struct sockaddr*)&sockaddr, &sockaddr_len))
    {
        int ret = ftp_perror(env);
        close(env->passive_fd);
        return ret;
    }
    port = sockaddr.sin_port;

    return ftp_active_printf(env,
        "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu).\r\n",
        (addr >> 0) & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF,
        (port >> 0) & 0xFF, (port >> 8) & 0xFF);
}

int ftp_cmd_CDUP(ftp_env_t* env, const char* arg)
{
    int pos = -1;
    for (size_t i = 0; i < sizeof(env->cwd); i++)
    {
        if (!env->cwd[i]) break;
        else if (env->cwd[i] == '/') pos = i;
    }

    if (pos <= 0) { env->cwd[0] = '/'; env->cwd[1] = '\0'; }
    else if (pos > 0) env->cwd[pos] = '\0';
    return ftp_active_printf(env, "250 OK\r\n");
}

int ftp_cmd_CHMOD(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    char* ptr;
    if (!arg[0] || !(ptr = strstr(arg, " ")))
        return ftp_active_printf(env, "501 Usage: CHMOD <MODE> <PATH>\r\n");

    mode_t mode = strtol(arg, 0, 8);
    ftp_abspath(env, pathbuf, ptr + 1);
    if (chmod(pathbuf, mode))
        return ftp_perror(env);
    return ftp_active_printf(env, "200 OK\r\n");
}

int ftp_cmd_CWD(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    struct stat st;

    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: CWD <PATH>\r\n");

    /* virtual /pkgs directory */
    if (!strcmp(arg, "/pkgs") || !strcmp(arg, "pkgs"))
    {
        strncpy(env->cwd, "/pkgs", sizeof(env->cwd) - 1);
        return ftp_active_printf(env, "250 OK (virtual /pkgs)\r\n");
    }

    ftp_abspath(env, pathbuf, arg);
    if (stat(pathbuf, &st))
        return ftp_perror(env);
    if (!S_ISDIR(st.st_mode))
        return ftp_active_printf(env, "550 No such directory\r\n");

    snprintf(env->cwd, sizeof(env->cwd), "%s", pathbuf);
    return ftp_active_printf(env, "250 OK\r\n");
}

int ftp_cmd_DELE(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: DELE <FILENAME>\r\n");
    ftp_abspath(env, pathbuf, arg);
    if (remove(pathbuf))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 File deleted\r\n");
}

int ftp_cmd_LIST(ftp_env_t* env, const char* arg)
{
    /* virtual /pkgs listing */
    if (!strcmp(env->cwd, "/pkgs"))
    {
        return ftp_send_pkg_listing(env);
    }

    char pathbuf[PATH_MAX + 256 + 2];
    struct dirent* ent;
    const char* p = env->cwd;
    struct stat statbuf;
    char timebuf[20];
    char modebuf[20];
    struct tm tm;
    DIR* dir;

    if (arg[0] && arg[0] != '-')
        p = arg;

    if (!(dir = opendir(p)))
        return ftp_perror(env);

    if (ftp_data_open(env))
        return ftp_perror(env);

    ftp_active_printf(env, "150 Opening data transfer\r\n");

    while ((ent = readdir(dir)))
    {
        if (p[0] == '/')
            snprintf(pathbuf, sizeof(pathbuf), "%s/%s", p, ent->d_name);
        else
            snprintf(pathbuf, sizeof(pathbuf), "/%s/%s/%s", env->cwd, p, ent->d_name);

        if (stat(pathbuf, &statbuf) != 0)
            continue;

        ftp_mode_string(statbuf.st_mode, modebuf);
        const uintptr_t pSt = (uintptr_t)&statbuf;
        LOCALTIME_R((const time_t*)(pSt + 0x38), &tm);
        strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", &tm);
        const uint64_t st_size = *(uint64_t*)(pSt + 0x48);

        if (env->show_title_id && has_pkg_ext(ent->d_name))
        {
            char tid[16];
            extract_title_id(ent->d_name, tid, sizeof(tid));
            if (tid[0])
            {
                ftp_data_printf(env, "%s %lu %lu %lu %llu %s %s [TID:%s]\r\n",
                    modebuf, 0, 0, 0, st_size, timebuf, ent->d_name, tid);
                continue;
            }
        }
        ftp_data_printf(env, "%s %lu %lu %lu %llu %s %s\r\n",
            modebuf, 0, 0, 0, st_size, timebuf, ent->d_name);
    }

    if (ftp_data_close(env))
    {
        int ret = ftp_perror(env);
        closedir(dir);
        return ret;
    }
    if (closedir(dir))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Transfer complete\r\n");
}

int ftp_cmd_MKD(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: MKD <DIRNAME>\r\n");
    ftp_abspath(env, pathbuf, arg);
    if (mkdir(pathbuf, 0777))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Directory created\r\n");
}

int ftp_cmd_NOOP(ftp_env_t* env, const char* arg)  { return ftp_active_printf(env, "200 NOOP OK\r\n"); }

int ftp_cmd_PORT(ftp_env_t* env, const char* arg)
{
    uint8_t addr[6];
    uint64_t s_addr;
    uint16_t port;
    if (sscanf(arg, "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu", addr, addr+1, addr+2, addr+3, addr+4, addr+5) != 6)
        return ftp_active_printf(env, "501 Usage: PORT <addr>\r\n");

    if ((env->data_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return ftp_perror(env);

    s_addr = (addr[3] << 24) | (addr[2] << 16) | (addr[1] << 8) | addr[0];
    port = (addr[5] << 8) | addr[4];

    env->data_addr.sin_family = AF_INET;
    env->data_addr.sin_addr.s_addr = s_addr;
    env->data_addr.sin_port = port;
    return ftp_active_printf(env, "200 PORT command successful.\r\n");
}

int ftp_cmd_PWD(ftp_env_t* env, const char* arg)   { return ftp_active_printf(env, "257 \"%s\"\r\n", env->cwd); }
int ftp_cmd_QUIT(ftp_env_t* env, const char* arg)  { ftp_active_printf(env, "221 Goodbye\r\n"); return -1; }

int ftp_cmd_REST(ftp_env_t* env, const char* arg)
{
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: REST <OFFSET>\r\n");
    env->data_offset = atol(arg);
    return ftp_active_printf(env, "350 REST OK\r\n");
}

int ftp_cmd_RETR(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    uint8_t buf[PAGE_SIZE];
    struct stat st;
    int err = 0, len, fd;

    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: RETR <PATH>\r\n");

    ftp_abspath(env, pathbuf, arg);
    if (stat(pathbuf, &st))
        return ftp_perror(env);
    if (S_ISDIR(st.st_mode))
        return ftp_active_printf(env, "550 Not a file\r\n");
    if ((fd = open(pathbuf, O_RDONLY, 0)) < 0)
        return ftp_active_printf(env, "550 %s\r\n", strerror(errno));
    if (ftp_active_printf(env, "150 Opening data transfer\r\n"))
    {
        close(fd);
        return -1;
    }
    if (ftp_data_open(env))
    {
        err = ftp_perror(env);
        close(fd);
        return err;
    }
    while ((len = read(fd, buf, sizeof(buf))) != 0)
    {
        if (len < 0 || len != write(env->data_fd, buf, len))
        {
            err = ftp_perror(env);
            ftp_data_close(env);
            close(fd);
            return err;
        }
    }
    close(fd);
    if (ftp_data_close(env))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Transfer completed\r\n");
}

int ftp_cmd_RMD(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: RMD <DIRNAME>\r\n");
    ftp_abspath(env, pathbuf, arg);
    if (rmdir(pathbuf))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Directory deleted\r\n");
}

int ftp_cmd_RNFR(ftp_env_t* env, const char* arg)
{
    struct stat st;
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: RNFR <PATH>\r\n");
    ftp_abspath(env, env->rename_path, arg);
    if (stat(env->rename_path, &st))
        return ftp_perror(env);
    return ftp_active_printf(env, "350 Awaiting new name\r\n");
}

int ftp_cmd_RNTO(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    struct stat st;
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: RNTO <PATH>\r\n");
    if (stat(env->rename_path, &st))
        return ftp_perror(env);
    ftp_abspath(env, pathbuf, arg);
    if (rename(env->rename_path, pathbuf))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Path renamed\r\n");
}

int ftp_cmd_SIZE(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    struct stat st;
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: SIZE <FILENAME>\r\n");
    ftp_abspath(env, pathbuf, arg);
    if (stat(pathbuf, &st))
        return ftp_perror(env);
    return ftp_active_printf(env, "213 %" PRIu64 "\r\n", st.st_size);
}

int ftp_cmd_STOR(ftp_env_t* env, const char* arg)
{
    off_t off = env->data_offset;
    uint8_t readbuf[0x4000];
    char pathbuf[PATH_MAX];
    int err = 0, fd;
    size_t len;

    env->data_offset = 0;
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: STOR <FILENAME>\r\n");

    ftp_abspath(env, pathbuf, arg);
    if ((fd = open(pathbuf, O_CREAT | O_WRONLY, 0777)) < 0)
        return ftp_perror(env);
    if (lseek(fd, off, SEEK_CUR) < 0)
    {
        err = ftp_perror(env);
        close(fd);
        return err;
    }
    if (ftp_active_printf(env, "150 Opening data transfer\r\n"))
    {
        close(fd);
        return -1;
    }
    if (ftp_data_open(env))
    {
        err = ftp_perror(env);
        close(fd);
        return err;
    }
    while ((len = ftp_data_read(env, readbuf, sizeof(readbuf))))
    {
        if (write(fd, readbuf, len) != len)
        {
            err = ftp_perror(env);
            ftp_data_close(env);
            close(fd);
            return err;
        }
        off += len;
    }
    if (ftruncate(fd, off))
    {
        err = ftp_perror(env);
        ftp_data_close(env);
        close(fd);
        return err;
    }
    close(fd);
    if (ftp_data_close(env))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 Data transfer complete\r\n");
}

int ftp_cmd_APPE(ftp_env_t* env, const char* arg)
{
    char pathbuf[PATH_MAX];
    struct stat statbuf;
    if (!arg[0])
        return ftp_active_printf(env, "501 Usage: APPE <FILENAME>\r\n");
    if (!env->data_offset)
    {
        ftp_abspath(env, pathbuf, arg);
        if (stat(pathbuf, &statbuf))
            return ftp_perror(env);
        env->data_offset = statbuf.st_size;
    }
    return ftp_cmd_STOR(env, arg);
}

int ftp_cmd_SYST(ftp_env_t* env, const char* arg)  { return ftp_active_printf(env, "215 UNIX Type: L8\r\n"); }

int ftp_cmd_TYPE(ftp_env_t* env, const char* arg)
{
    switch (arg[0])
    {
    case 'A':
    case 'I':
        env->type = arg[0];
        return ftp_active_printf(env, "200 Type set to %c\r\n", env->type);
    }
    return ftp_active_printf(env, "501 Invalid argument to TYPE\r\n");
}

int ftp_cmd_USER(ftp_env_t* env, const char* arg)  { return ftp_active_printf(env, "230 User logged in\r\n"); }

int ftp_cmd_KILL(ftp_env_t* env, const char* arg)
{
    FTP_LOG_PUTS("Server killed");
    exit(EXIT_SUCCESS);
    return -1;
}

int ftp_cmd_unavailable(ftp_env_t* env, const char* arg) { return ftp_active_printf(env, "502 Command not implemented\r\n"); }
int ftp_cmd_unknown(ftp_env_t* env, const char* arg)     { return ftp_active_printf(env, "502 Command not recognized\r\n"); }

int ftp_cmd_MTRW(ftp_env_t* env, const char* arg)
{
#ifdef __PROSPERO__
    struct iovec iov_sys[] = {
        IOVEC_ENTRY("from"), IOVEC_ENTRY("/dev/ssd0.system"),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY("/system"),
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"), IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"), IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"), IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    struct iovec iov_sysex[] = {
        IOVEC_ENTRY("from"), IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"), IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"), IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"), IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    if (syscall(SYS_nmount, iov_sys, IOVEC_SIZE(iov_sys), MNT_UPDATE))
        return ftp_perror(env);
    if (syscall(SYS_nmount, iov_sysex, IOVEC_SIZE(iov_sysex), MNT_UPDATE))
        return ftp_perror(env);
    return ftp_active_printf(env, "226 /system and /system_ex remounted\r\n");
#else
    return ftp_active_printf(env, "226 command not supported\r\n");
#endif
}

/* NEW: SITE PKGS — plain-text list of all found PKGs, no data connection */
int ftp_cmd_PKGS(ftp_env_t* env, const char* arg)
{
    pkg_list_t list;
    scan_all_usb(&list);

    ftp_active_printf(env, "200-%d PKG(s) found\r\n", list.count);
    for (int i = 0; i < list.count; i++)
    {
        pkg_entry_t* e = &list.entries[i];
        if (env->show_title_id && e->title_id[0])
            ftp_active_printf(env, " %s [TID:%s] %" PRIu64 " bytes\r\n", e->path, e->title_id, e->size);
        else
            ftp_active_printf(env, " %s %" PRIu64 " bytes\r\n", e->path, e->size);
    }
    if (list.overflow)
        ftp_active_printf(env, " # truncated at %d entries\r\n", PKG_MAX_RESULTS);

    return ftp_active_printf(env, "200 End of PKG list\r\n");
}
