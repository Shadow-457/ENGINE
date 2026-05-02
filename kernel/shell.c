#include "../include/kernel.h"

#define SHELL_MAX_LINE 1024
#define SHELL_MAX_ARGS 64
#define SHELL_MAX_CMDS 16
#define SHELL_VAR_MAX 32
#define SHELL_VAR_NAME_MAX 32
#define SHELL_VAR_VAL_MAX 256
#define MAX_PATH 256

typedef struct {
    char name[SHELL_VAR_NAME_MAX];
    char value[SHELL_VAR_VAL_MAX];
} shell_var_t;

typedef struct {
    int argc;
    char *argv[SHELL_MAX_ARGS];
    char *input_redirect;
    char *output_redirect;
    int append_output;
    int background;
} shell_cmd_t;

typedef struct {
    int cmd_count;
    shell_cmd_t cmds[SHELL_MAX_CMDS];
    int pipe_count;
} shell_pipeline_t;

static shell_var_t shell_vars[SHELL_VAR_MAX];
static int shell_var_count = 0;
static char shell_cwd[MAX_PATH] = "/";
static char shell_line[SHELL_MAX_LINE];

static const char *shell_prompt = "systrix$ ";

void shell_init(void) {
    memset(shell_vars, 0, sizeof(shell_vars));
    shell_var_count = 0;
    memset(shell_cwd, 0, sizeof(shell_cwd));
    shell_cwd[0] = '/';
}

static const char *shell_get_var(const char *name) {
    for (int i = 0; i < shell_var_count; i++) {
        if (strcmp(shell_vars[i].name, name) == 0) {
            return shell_vars[i].value;
        }
    }
    return NULL;
}

static void shell_set_var(const char *name, const char *value) {
    for (int i = 0; i < shell_var_count; i++) {
        if (strcmp(shell_vars[i].name, name) == 0) {
            /* strlcpy always NUL-terminates and won't overflow the slot */
            strlcpy(shell_vars[i].value, value, SHELL_VAR_VAL_MAX);
            return;
        }
    }
    if (shell_var_count < SHELL_VAR_MAX) {
        strlcpy(shell_vars[shell_var_count].name,  name,  SHELL_VAR_NAME_MAX);
        strlcpy(shell_vars[shell_var_count].value, value, SHELL_VAR_VAL_MAX);
        shell_var_count++;
    }
}

static void shell_expand_vars(char *line, char *out, usize max_len) {
    usize i = 0, j = 0;
    while (line[i] && j < max_len - 1) {
        if (line[i] == '$' && line[i + 1] == '{') {
            i += 2;
            char varname[SHELL_VAR_NAME_MAX];
            int k = 0;
            while (line[i] && line[i] != '}' && k < SHELL_VAR_NAME_MAX - 1)
                varname[k++] = line[i++];
            varname[k] = '\0';
            if (line[i] == '}') i++;
            const char *val = shell_get_var(varname);
            if (val) {
                /* Copy val into out[j..], respecting the output budget */
                usize room = max_len - 1 - j;
                usize vlen = strnlen(val, room);
                memcpy(out + j, val, vlen);
                j += vlen;
            }
        } else {
            out[j++] = line[i++];
        }
    }
    out[j] = '\0';
}

static int shell_parse_line(const char *line, shell_pipeline_t *pipeline) {
    memset(pipeline, 0, sizeof(shell_pipeline_t));
    char expanded[SHELL_MAX_LINE];
    shell_expand_vars((char*)line, expanded, SHELL_MAX_LINE);
    char *p = expanded;
    pipeline->cmd_count = 0;
    pipeline->pipe_count = 0;
    while (*p) {
        while (isspace((unsigned char)*p) && *p != '\n') p++;
        if (!*p) break;
        if (pipeline->cmd_count >= SHELL_MAX_CMDS) break;
        shell_cmd_t *cmd = &pipeline->cmds[pipeline->cmd_count];
        cmd->argc = 0;
        cmd->input_redirect = NULL;
        cmd->output_redirect = NULL;
        cmd->append_output = 0;
        cmd->background = 0;
        while (*p && *p != '|' && *p != '>' && *p != '<' && *p != '&' && *p != '\n') {
            while (isspace((unsigned char)*p) && *p != '\n') p++;
            if (!*p || *p == '|' || *p == '>' || *p == '<' || *p == '&') break;
            if (cmd->argc >= SHELL_MAX_ARGS - 1) break;
            cmd->argv[cmd->argc] = p;
            cmd->argc++;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '>' && *p != '<' && *p != '&') p++;
            if (isspace((unsigned char)*p) && *p != '\n') { *p = 0; p++; }
        }
        cmd->argv[cmd->argc] = NULL;
        if (*p == '|') {
            pipeline->pipe_count++;
            p++;
            pipeline->cmd_count++;
        } else if (*p == '>') {
            p++;
            if (*p == '>') { cmd->append_output = 1; p++; }
            while (isspace((unsigned char)*p) && *p != '\n') p++;
            cmd->output_redirect = p;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&') p++;
            if (*p) { *p = 0; p++; }
        } else if (*p == '<') {
            p++;
            while (isspace((unsigned char)*p) && *p != '\n') p++;
            cmd->input_redirect = p;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&') p++;
            if (*p) { *p = 0; p++; }
        } else if (*p == '&') {
            cmd->background = 1;
            p++;
        } else if (*p == '\n' || *p == 0) {
            break;
        }
        pipeline->cmd_count++;
    }
    return pipeline->cmd_count;
}

static i64 shell_exec_cmd(shell_cmd_t *cmd) {
    if (cmd->argc == 0) return 0;
    if (strcmp(cmd->argv[0], "echo") == 0) {
        for (int i = 1; i < cmd->argc; i++) {
            kprintf("%s", cmd->argv[i]);
            if (i < cmd->argc - 1) kprintf(" ");
        }
        kprintf("\r\n");
        return 0;
    }
    if (strcmp(cmd->argv[0], "cd") == 0) {
        if (cmd->argc > 1) {
            const char *target = cmd->argv[1];
            char new_cwd[MAX_PATH];
            /* Join cwd + target, then normalize to handle ".." and "." */
            if (slibc_path_join(new_cwd, sizeof(new_cwd), shell_cwd, target) < 0) {
                kprintf("cd: path too long\r\n");
                return (i64)ENAMETOOLONG;
            }
            slibc_path_normalize(new_cwd);
            strlcpy(shell_cwd, new_cwd, MAX_PATH);
        } else {
            /* cd with no args goes to root */
            shell_cwd[0] = '/'; shell_cwd[1] = '\0';
        }
        return 0;
    }
    if (strcmp(cmd->argv[0], "pwd") == 0) {
        kprintf("%s", shell_cwd);
        kprintf("\r\n");
        return 0;
    }
    if (strcmp(cmd->argv[0], "ls") == 0) {
        kprintf("Directory listing not yet implemented\r\n");
        return 0;
    }
    if (strcmp(cmd->argv[0], "cat") == 0) {
        if (cmd->argc < 2) return (i64)EINVAL;
        i64 fd = vfs_open(cmd->argv[1]);
        if (fd < 0) return fd;
        char buf[512];
        i64 r;
        while ((r = vfs_read(fd, buf, 512)) > 0) {
            for (i64 i = 0; i < r; i++) vga_putchar(buf[i]);
        }
        vfs_close(fd);
        kprintf("\r\n");
        return 0;
    }
    if (strcmp(cmd->argv[0], "ps") == 0) {
        ps_list();
        return 0;
    }
    if (strcmp(cmd->argv[0], "export") == 0) {
        if (cmd->argc >= 3 && strcmp(cmd->argv[1], "=") == 0) {
            shell_set_var(cmd->argv[0], cmd->argv[2]);
        } else if (cmd->argc >= 2) {
            char *eq = strchr(cmd->argv[1], '=');
            if (eq) {
                *eq = 0;
                shell_set_var(cmd->argv[1], eq + 1);
            }
        }
        return 0;
    }
    if (strcmp(cmd->argv[0], "env") == 0) {
        for (int i = 0; i < shell_var_count; i++) {
            kprintf("%s", shell_vars[i].name);
            kprintf("=");
            kprintf("%s", shell_vars[i].value);
            kprintf("\r\n");
        }
        return 0;
    }
    if (strcmp(cmd->argv[0], "help") == 0) {
        kprintf("Commands: echo, cd, pwd, ls, cat, ps, ipc, export, env,\r\n          lspci, lsusb, help, exit\r\n");
        return 0;
    }
    if (strcmp(cmd->argv[0], "lspci") == 0) {
        pci_list_devices();
        return 0;
    }
    if (strcmp(cmd->argv[0], "lsusb") == 0) {
        usb_list_devices();
        return 0;
    }
    if (strcmp(cmd->argv[0], "ipc") == 0) {
        ipc_dump_servers();
        return 0;
    }
    if (strcmp(cmd->argv[0], "exit") == 0) {
        return -1;
    }
    i64 fd = vfs_open(cmd->argv[0]);
    if (fd < 0) {
        kprintf("Command not found: ");
        kprintf("%s", cmd->argv[0]);
        kprintf("\r\n");
        return (i64)ENOENT;
    }
    /* Get real file size: seek to end, then seek back to start */
    i64 sz = vfs_seek(fd, 0, 2); /* SEEK_END */
    if (sz <= 0) { vfs_close(fd); return (i64)ENOENT; }
    vfs_seek(fd, 0, 0); /* SEEK_SET */
    char *data = (char*)sys_malloc((usize)sz);
    if (!data) { vfs_close(fd); return (i64)ENOMEM; }
    sz = vfs_read(fd, data, (usize)sz);
    vfs_close(fd);
    if (sz <= 0) { sys_free(data); return (i64)ENOENT; }
    Elf64Hdr *eh = (Elf64Hdr*)data;
    if (eh->magic != ELF_MAGIC) { sys_free(data); return (i64)EINVAL; }
    /* elf_load() creates the process, maps all PT_LOAD segments and
     * the user stack into its own CR3, then returns the pid.
     * process_create() alone only allocates a PCB — it never maps
     * the ELF segments, so the process would fault the moment it
     * tried to execute its first instruction. */
    i64 pid = elf_load(data, (usize)sz, cmd->argv[0]);
    sys_free(data);
    if (pid < 0) return pid;
    process_run((u64)pid);
    return pid;
}

static i64 shell_exec_pipeline(shell_pipeline_t *pipeline) {
    if (pipeline->cmd_count == 0) return 0;
    if (pipeline->cmd_count == 1) {
        return shell_exec_cmd(&pipeline->cmds[0]);
    }
    int pipefd[SHELL_MAX_CMDS - 1][2];
    for (int i = 0; i < pipeline->pipe_count; i++) {
        int pfd[2];
        i64 r = sys_pipe(pfd);
        if (r < 0) return r;
        pipefd[i][0] = pfd[0];
        pipefd[i][1] = pfd[1];
    }
    for (int i = 0; i < pipeline->cmd_count; i++) {
        if (i > 0) {
            vfs_dup2(pipefd[i - 1][0], 0);
            vfs_close(pipefd[i - 1][0]);
            vfs_close(pipefd[i - 1][1]);
        }
        if (i < pipeline->cmd_count - 1) {
            vfs_dup2(pipefd[i][1], 1);
            vfs_close(pipefd[i][0]);
            vfs_close(pipefd[i][1]);
        }
        i64 r = shell_exec_cmd(&pipeline->cmds[i]);
        if (r < 0) return r;
    }
    return 0;
}

void shell_run(void) {
    shell_init();
    kprintf("\r\nSystrix OS Shell v0.1\r\nType 'help' for commands\r\n\r\n");
    while (1) {
        kprintf("systrix:");
        kprintf("%s", shell_cwd);
        kprintf("%s", shell_prompt);
        int pos = 0;
        memset(shell_line, 0, SHELL_MAX_LINE);
        while (1) {
            u8 c = read_key_raw();
            if (c == '\r' || c == '\n') {
                shell_line[pos] = 0;
                kprintf("\r\n");
                break;
            }
            if (c == '\b' && pos > 0) {
                pos--;
                shell_line[pos] = 0;
                vga_putchar('\b'); vga_putchar(' '); vga_putchar('\b');
                continue;
            }
            if (c >= 0x20 && pos < SHELL_MAX_LINE - 1) {
                shell_line[pos++] = c;
                vga_putchar(c);
            }
        }
        if (shell_line[0] == 0) continue;
        shell_pipeline_t pipeline;
        shell_parse_line(shell_line, &pipeline);
        i64 r = shell_exec_pipeline(&pipeline);
        if (r == -1) break;
    }
}
