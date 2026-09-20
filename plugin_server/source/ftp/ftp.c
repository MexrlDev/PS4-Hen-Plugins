/* Copyright (C) 2023 John Törnblom
 * Copyright (C) 2026 PurpyHen contributors
 *
 * GPLv3 or later.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/syscall.h>

#include "cmd.h"
#include "log.h"
#include "../../../common/notify.h"
#include "../../../common/path.h"
#include "../../../common/ini.h"
#include "../../../common/file.h"

typedef struct ftp_command
{
    const char* name;
    ftp_command_fn_t* func;
} ftp_command_t;

static ftp_command_t commands[] = {
    {"APPE", ftp_cmd_APPE},
    {"CDUP", ftp_cmd_CDUP},
    {"CWD",  ftp_cmd_CWD},
    {"DELE", ftp_cmd_DELE},
    {"LIST", ftp_cmd_LIST},
    {"MKD",  ftp_cmd_MKD},
    {"NOOP", ftp_cmd_NOOP},
    {"PASV", ftp_cmd_PASV},
    {"PORT", ftp_cmd_PORT},
    {"PWD",  ftp_cmd_PWD},
    {"QUIT", ftp_cmd_QUIT},
    {"REST", ftp_cmd_REST},
    {"RETR", ftp_cmd_RETR},
    {"RMD",  ftp_cmd_RMD},
    {"RNFR", ftp_cmd_RNFR},
    {"RNTO", ftp_cmd_RNTO},
    {"SIZE", ftp_cmd_SIZE},
    {"STOR", ftp_cmd_STOR},
    {"SYST", ftp_cmd_SYST},
    {"TYPE", ftp_cmd_TYPE},
    {"USER", ftp_cmd_USER},

    /* custom */
    {"KILL",  ftp_cmd_KILL},
    {"MTRW",  ftp_cmd_MTRW},
    {"CHMOD", ftp_cmd_CHMOD},
    {"PKGS",  ftp_cmd_PKGS},   /* NEW: SITE PKGS */

    /* 4-byte aliases */
    {"XCUP", ftp_cmd_CWD},
    {"XMKD", ftp_cmd_MKD},
    {"XPWD", ftp_cmd_PWD},
    {"XRMD", ftp_cmd_RMD},

    {"XRCP", ftp_cmd_unavailable},
    {"XRSQ", ftp_cmd_unavailable},
    {"XSEM", ftp_cmd_unavailable},
    {"XSEN", ftp_cmd_unavailable},
};

static int nb_ftp_commands = (sizeof(commands) / sizeof(ftp_command_t));

/* ------------------------------------------------------------------ */
/* Read hen.ini once per new connection to snapshot PurpyHen settings  */
/* ------------------------------------------------------------------ */
static int ini_get_bool(const INIFile* ini, const char* key, int fallback)
{
    if (!ini) return fallback;
    char* v = ini_get((INIFile*)ini, HEN_SECTION, key);
    if (!v || !v[0]) return fallback;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
        return 1;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") ||
        !strcasecmp(v, "no")  || !strcasecmp(v, "off"))
        return 0;
    return fallback;
}

static void load_purpyhen_settings(int* show_title_id, int* show_network_info)
{
    *show_title_id = 1;
    *show_network_info = 0;

    INIFile* ini = ini_load(HDD_INI_PATH);
    if (!ini)
        return;

    *show_title_id     = ini_get_bool(ini, "show_title_id", 1);
    *show_network_info = ini_get_bool(ini, "show_network_info", 0);

    ini_free(ini);
}

/* ------------------------------------------------------------------ */

static char* ftp_readline(int fd)
{
    int bufsize = 1024;
    int position = 0;
    char* buffer_backup;
    char* buffer = calloc(bufsize, sizeof(char));
    char c;

    if (!buffer) { FTP_LOG_PERROR("malloc"); return NULL; }

    while (1)
    {
        int len = read(fd, &c, 1);
        if (len == -1 && errno == EINTR) continue;
        if (len <= 0) { free(buffer); return NULL; }

        if (c == '\r') { buffer[position] = '\0'; position = 0; continue; }
        if (c == '\n') return buffer;

        buffer[position++] = c;
        if (position >= bufsize)
        {
            bufsize += 1024;
            buffer_backup = buffer;
            buffer = realloc(buffer, bufsize);
            if (!buffer) { FTP_LOG_PERROR("realloc"); free(buffer_backup); return NULL; }
        }
    }
}

static int ftp_execute(ftp_env_t* env, char* line)
{
    char* sep = strchr(line, ' ');
    char* arg = strchr(line, 0);

    if (sep) { sep[0] = 0; arg = sep + 1; }

    for (int i = 0; i < nb_ftp_commands; i++)
    {
        if (strcmp(line, commands[i].name)) continue;
        return commands[i].func(env, arg);
    }
    return ftp_cmd_unknown(env, arg);
}

static int ftp_greet(ftp_env_t* env)
{
    char msg[0x200];
    snprintf(msg, sizeof(msg),
             "220-Welcome to PurpyHen FTP (pid %d, built %s %s)\r\n"
             "220-Commands include SITE PKGS to list all USB PKGs\r\n"
             "220 Service is ready\r\n",
             getpid(), __DATE__, __TIME__);
    size_t len = strlen(msg);
    if (write(env->active_fd, msg, len) != len) return -1;
    return 0;
}

static void* ftp_thread(void* args)
{
    ftp_env_t env;
    bool running;
    char* line;
    char* cmd;

    env.data_fd = -1;
    env.passive_fd = -1;
    env.active_fd = (int)(long)args;
    env.type = 'A';
    env.data_offset = 0;
    env.show_title_id = 1;

    int show_net = 0;
    load_purpyhen_settings(&env.show_title_id, &show_net);

    strcpy(env.cwd, "/");
    memset(env.rename_path, 0, sizeof(env.rename_path));
    memset(&env.data_addr, 0, sizeof(env.data_addr));

    running = !ftp_greet(&env);
    while (running)
    {
        if (!(line = ftp_readline(env.active_fd))) break;
        cmd = line;
        if (!strncmp(line, "SITE ", 5)) cmd += 5;
        if (ftp_execute(&env, cmd)) running = false;
        free(line);
    }

    if (env.active_fd > 0) close(env.active_fd);
    if (env.passive_fd > 0) close(env.passive_fd);
    if (env.data_fd > 0) close(env.data_fd);
    pthread_exit(NULL);
    return NULL;
}

static int ftp_serve(uint16_t port, int notify_user)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    char ip[INET_ADDRSTRLEN];
    struct ifaddrs* ifaddr;
    int ifaddr_wait = 1;
    socklen_t addr_len;
    pthread_t trd;
    int connfd;
    int srvfd;

    if (getifaddrs(&ifaddr) == -1)
    {
        FTP_LOG_PERROR("getifaddrs");
        exit(EXIT_FAILURE);
    }
    signal(SIGPIPE, SIG_IGN);

    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!strncmp("lo", ifa->ifa_name, 2)) continue;

        struct sockaddr_in* in = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));

        if (!strncmp("0.", ip, 2)) continue;

        if (notify_user)
        {
            Notify(TEX_ICON_SYSTEM,
                   "Serving FTP on\n"
                   "%s:%d (%s)\n"
                   "Try `SITE PKGS` for PKG list\n"
                   "Built %s %s",
                   ip, port, ifa->ifa_name, __DATE__, __TIME__);
        }
        ifaddr_wait = 0;
    }
    freeifaddrs(ifaddr);

    if (ifaddr_wait) return 0;

    if ((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { FTP_LOG_PERROR("socket"); return -1; }
    if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
    { FTP_LOG_PERROR("setsockopt"); return -1; }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(srvfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0)
    { FTP_LOG_PERROR("bind"); return -1; }
    if (listen(srvfd, 5) != 0) { FTP_LOG_PERROR("listen"); return -1; }

    addr_len = sizeof(client_addr);
    while (1)
    {
        if ((connfd = accept(srvfd, (struct sockaddr*)&client_addr, &addr_len)) < 0)
        { FTP_LOG_PERROR("accept"); break; }
        pthread_create(&trd, NULL, ftp_thread, (void*)(long)connfd);
    }
    return close(srvfd);
}

int ftp_main(void)
{
    uint16_t port = 2121;
    int notify_user = 1;

    printf("PurpyHen FTP server built at %s %s\n", __DATE__, __TIME__);

    /* Decide once per server start whether to spam notifications */
    int show_title_id = 1, show_network_info = 0;
    load_purpyhen_settings(&show_title_id, &show_network_info);

    while (1)
    {
        ftp_serve(port, show_network_info ? notify_user : 0);
        notify_user = 0;
        sleep(3);
    }
    return EXIT_SUCCESS;
}
