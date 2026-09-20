/* Copyright (C) 2023 John Törnblom
 * Copyright (C) 2026 PurpyHen contributors
 *
 * GPLv3 or later.
 */

#pragma once

#include <limits.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <unistd.h>

typedef struct ftp_env
{
    int data_fd;
    int active_fd;
    int passive_fd;
    char cwd[PATH_MAX];

    char type;
    off_t data_offset;
    char rename_path[PATH_MAX];
    struct sockaddr_in data_addr;

    /* NEW: config snapshot so we don't re-parse hen.ini per command */
    int show_title_id;
} ftp_env_t;

typedef int(ftp_command_fn_t)(ftp_env_t* env, const char* arg);

/* Standard FTP commands */
int ftp_cmd_APPE(ftp_env_t* env, const char* arg);
int ftp_cmd_CDUP(ftp_env_t* env, const char* arg);
int ftp_cmd_CWD(ftp_env_t* env, const char* arg);
int ftp_cmd_DELE(ftp_env_t* env, const char* arg);
int ftp_cmd_LIST(ftp_env_t* env, const char* arg);
int ftp_cmd_MKD(ftp_env_t* env, const char* arg);
int ftp_cmd_NOOP(ftp_env_t* env, const char* arg);
int ftp_cmd_PASV(ftp_env_t* env, const char* arg);
int ftp_cmd_PORT(ftp_env_t* env, const char* arg);
int ftp_cmd_PWD(ftp_env_t* env, const char* arg);
int ftp_cmd_QUIT(ftp_env_t* env, const char* arg);
int ftp_cmd_REST(ftp_env_t* env, const char* arg);
int ftp_cmd_RETR(ftp_env_t* env, const char* arg);
int ftp_cmd_RMD(ftp_env_t* env, const char* arg);
int ftp_cmd_RNFR(ftp_env_t* env, const char* arg);
int ftp_cmd_RNTO(ftp_env_t* env, const char* arg);
int ftp_cmd_SIZE(ftp_env_t* env, const char* arg);
int ftp_cmd_STOR(ftp_env_t* env, const char* arg);
int ftp_cmd_SYST(ftp_env_t* env, const char* arg);
int ftp_cmd_TYPE(ftp_env_t* env, const char* arg);
int ftp_cmd_USER(ftp_env_t* env, const char* arg);

/* Custom FTP commands */
int ftp_cmd_KILL(ftp_env_t* env, const char* arg);
int ftp_cmd_MTRW(ftp_env_t* env, const char* arg);
int ftp_cmd_CHMOD(ftp_env_t* env, const char* arg);
int ftp_cmd_PKGS(ftp_env_t* env, const char* arg);   /* NEW: SITE PKGS */

/* Errors */
int ftp_cmd_unavailable(ftp_env_t* env, const char* arg);
int ftp_cmd_unknown(ftp_env_t* env, const char* arg);
