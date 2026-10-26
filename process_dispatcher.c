/*
 * CP386 Assignment 2 - Question 2: process_dispatcher.c
 * ------------------------------------------------------- 
 * Author: Youssef Fawzy, Adam Rak
 * -------------------------------------------------------
 * ID: 169021393, 210700280
 * -------------------------------------------------------
 * Email: fawz1393@mylaurier.ca, rakx0280@mylaurier.ca
 * -------------------------------------------------------
 * Date: 26-October-2026
 * -------------------------------------------------------                                                       
 * 
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAX_ARGS 64
#define SHM_NAME_LEN 64

/* ------------------------------------------------------------------ */
/* Provided output function (called by the Parent for every job)       */
/* ------------------------------------------------------------------ */
static int writeOutput(const char *command, const char *output)
{
    FILE *fp = fopen("output.txt", "a");
    if (fp == NULL) {
        perror("fopen");
        return -1;}
    fprintf(fp, "The output of: %s : is\n", command);
    fprintf(fp, ">>>>>>>>>>>>>>>\n%s<<<<<<<<<<<<<<<\n", output);
    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Used in children: _exit skips stdio cleanup, which is safe after fork(). */
static void die(const char *msg)
{
    perror(msg);
    _exit(EXIT_FAILURE);
}

/* Growable string buffer */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_append(Buf *b, const char *src, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : 256;
        while (newcap < b->len + n + 1)
            newcap *= 2;
        char *tmp = realloc(b->data, newcap);
        if (tmp == NULL)
            die("realloc");
        b->data = tmp;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* Write the whole buffer, handling partial writes and EINTR. */
static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            die("write");
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* Read from fd until EOF into b (always NUL-terminated). */
static void read_until_eof(int fd, Buf *b)
{
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            die("read");
        }
        if (n == 0)
            break;
        buf_append(b, chunk, (size_t)n);
    }
    if (b->data == NULL)           /* nothing was read: return an empty string */
        buf_append(b, "", 0);
}

/* Append "Stage N (name): exit X" (or the signal) for one finished child. */
static void append_status(Buf *b, int stage, const char *name, int status)
{
    char line[256];
    if (WIFEXITED(status))
        snprintf(line, sizeof line, "Stage %d (%s): exit %d\n",
                 stage, name, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(line, sizeof line, "Stage %d (%s): killed by signal %d\n",
                 stage, name, WTERMSIG(status));
    else
        snprintf(line, sizeof line, "Stage %d (%s): unknown status\n", stage, name);
    buf_append(b, line, strlen(line));
}

/* Split a command string (whitespace separated) into a NULL-terminated argv. */
static int build_args(char *command, char *args[])
{
    int i = 0;
    char *save = NULL;
    for (char *tok = strtok_r(command, " \t", &save);
         tok != NULL && i < MAX_ARGS - 1;
         tok = strtok_r(NULL, " \t", &save))
        args[i++] = tok;
    args[i] = NULL;
    return i;
}

/* ------------------------------------------------------------------ */
/* Reader Child: input file -> POSIX shared memory                     */
/* ------------------------------------------------------------------ */
static void reader_process(const char *filename, const char *shm_name)
{
    FILE *fp = fopen(filename, "r");
    if (fp == NULL)
        die(filename);

    /* Find the file size so the shared-memory object is big enough. */
    if (fseek(fp, 0, SEEK_END) != 0)
        die("fseek");
    long size = ftell(fp);
    if (size < 0)
        die("ftell");
    rewind(fp);

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd == -1)
        die("shm_open (reader)");

    /* +1 for a terminating '\0' */
    if (ftruncate(fd, (off_t)size + 1) == -1) {
        shm_unlink(shm_name);
        die("ftruncate");
    }

    char *ptr = mmap(NULL, (size_t)size + 1, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        shm_unlink(shm_name);
        die("mmap (reader)");
    }

    size_t got = fread(ptr, 1, (size_t)size, fp);
    ptr[got] = '\0';
    fclose(fp);

    munmap(ptr, (size_t)size + 1);
    close(fd);
    _exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Manager Child: runs one job (1 or 2 commands)                       */
/* ------------------------------------------------------------------ */
static void manager_process(char *cmd1[], char *cmd2[], int result_fd)
{
    int two = (cmd2 != NULL);
    int cmd_pipe[2] = { -1, -1 };   /* Command Child 1 -> Command Child 2 */
    int out_pipe[2];                /* last Command Child -> Manager      */
    pid_t pids[2] = { -1, -1 };

    if (two && pipe(cmd_pipe) == -1)
        die("pipe (command pipe)");
    if (pipe(out_pipe) == -1)
        die("pipe (output pipe)");

    /* ---- Command Child 1 ---- */
    pids[0] = fork();
    if (pids[0] == -1)
        die("fork");
    if (pids[0] == 0) {
        close(result_fd);
        close(out_pipe[0]);
        if (two) {
            close(cmd_pipe[0]);
            if (dup2(cmd_pipe[1], STDOUT_FILENO) == -1)
                die("dup2");
            close(cmd_pipe[1]);
            close(out_pipe[1]);
        } else {
            if (dup2(out_pipe[1], STDOUT_FILENO) == -1)
                die("dup2");
            close(out_pipe[1]);
        }
        execvp(cmd1[0], cmd1);
        _exit(127);                 /* command not found / exec failed */
    }

    /* ---- Command Child 2 (only for "cmd1 | cmd2") ---- */
    if (two) {
        pids[1] = fork();
        if (pids[1] == -1)
            die("fork");
        if (pids[1] == 0) {
            close(result_fd);
            close(cmd_pipe[1]);
            close(out_pipe[0]);
            if (dup2(cmd_pipe[0], STDIN_FILENO) == -1)
                die("dup2");
            if (dup2(out_pipe[1], STDOUT_FILENO) == -1)
                die("dup2");
            close(cmd_pipe[0]);
            close(out_pipe[1]);
            execvp(cmd2[0], cmd2);
            _exit(127);
        }
    }

    /* Manager keeps only the read end of the output pipe. */
    if (two) {
        close(cmd_pipe[0]);
        close(cmd_pipe[1]);
    }
    close(out_pipe[1]);

    Buf result = { NULL, 0, 0 };
    read_until_eof(out_pipe[0], &result);
    close(out_pipe[0]);

    /* Collect the Command Child(ren) and record exit statuses. */
    int status[2] = { 0, 0 };
    for (int i = 0; i < (two ? 2 : 1); i++) {
        if (waitpid(pids[i], &status[i], 0) == -1)
            die("waitpid");
    }

    buf_append(&result, "[Process status]\n", strlen("[Process status]\n"));
    append_status(&result, 1, cmd1[0], status[0]);
    if (two)
        append_status(&result, 2, cmd2[0], status[1]);

    /* Return output + exit statuses to the Parent through the result pipe. */
    write_all(result_fd, result.data, result.len);
    close(result_fd);
    free(result.data);
    _exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Parent helpers                                                      */
/* ------------------------------------------------------------------ */

/* Run one job through a Manager Child; returns the result string (malloc'd). */
static char *run_job(char *cmd1[], char *cmd2[])
{
    int result_pipe[2];
    if (pipe(result_pipe) == -1) {
        perror("pipe (result pipe)");
        return NULL;
    }

    fflush(NULL);
    pid_t mgr = fork();
    if (mgr == -1) {
        perror("fork (manager)");
        close(result_pipe[0]);
        close(result_pipe[1]);
        return NULL;
    }
    if (mgr == 0) {
        close(result_pipe[0]);
        manager_process(cmd1, cmd2, result_pipe[1]);   /* never returns */
    }

    close(result_pipe[1]);

    Buf result = { NULL, 0, 0 };
    /* read_until_eof exits via die() on error, which is acceptable here */
    read_until_eof(result_pipe[0], &result);
    close(result_pipe[0]);

    int status;
    waitpid(mgr, &status, 0);
    return result.data;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Start with an empty output.txt (only the Parent ever writes to it). */
    FILE *clear = fopen("output.txt", "w");
    if (clear == NULL) {
        perror("fopen output.txt");
        return EXIT_FAILURE;
    }
    fclose(clear);

    char shm_name[SHM_NAME_LEN];
    snprintf(shm_name, sizeof shm_name, "/process_dispatcher_%d", (int)getpid());

    /* ---- 1. Reader Child puts the file contents into shared memory ---- */
    fflush(NULL);
    pid_t reader = fork();
    if (reader == -1) {
        perror("fork (reader)");
        return EXIT_FAILURE;
    }
    if (reader == 0)
        reader_process(argv[1], shm_name);   /* never returns */

    int rstatus;
    if (waitpid(reader, &rstatus, 0) == -1) {
        perror("waitpid (reader)");
        shm_unlink(shm_name);
        return EXIT_FAILURE;
    }
    if (!WIFEXITED(rstatus) || WEXITSTATUS(rstatus) != 0) {
        fprintf(stderr, "Reader child failed; could not read %s\n", argv[1]);
        shm_unlink(shm_name);
        return EXIT_FAILURE;
    }

    /* ---- 2. Copy shared memory into a dynamically allocated array ---- */
    int shm_fd = shm_open(shm_name, O_RDONLY, 0600);
    if (shm_fd == -1) {
        perror("shm_open (parent)");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(shm_fd, &st) == -1) {
        perror("fstat");
        close(shm_fd);
        shm_unlink(shm_name);
        return EXIT_FAILURE;
    }
    size_t shm_size = (size_t)st.st_size;

    char *shm_ptr = mmap(NULL, shm_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap (parent)");
        close(shm_fd);
        shm_unlink(shm_name);
        return EXIT_FAILURE;
    }

    char *workload = malloc(shm_size + 1);
    if (workload == NULL) {
        perror("malloc");
        munmap(shm_ptr, shm_size);
        close(shm_fd);
        shm_unlink(shm_name);
        return EXIT_FAILURE;
    }
    memcpy(workload, shm_ptr, shm_size);
    workload[shm_size] = '\0';

    /* Copy done: unmap and remove the shared-memory object. */
    munmap(shm_ptr, shm_size);
    close(shm_fd);
    shm_unlink(shm_name);

    /* ---- 3. Split into jobs and run them one by one, in file order ---- */
    char *save = NULL;
    for (char *line = strtok_r(workload, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        /* trim leading/trailing whitespace (including '\r') */
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                           line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;                         /* skip blank lines */

        /* keep an untouched copy for the "The output of: ..." header */
        char *original = strdup(line);
        if (original == NULL) {
            perror("strdup");
            break;
        }

        /* a job has at most one '|' */
        char *bar = strchr(line, '|');
        char *left = line;
        char *right = NULL;
        if (bar != NULL) {
            *bar = '\0';
            right = bar + 1;
        }

        char *args1[MAX_ARGS];
        char *args2[MAX_ARGS];
        int n1 = build_args(left, args1);
        int n2 = right ? build_args(right, args2) : 0;

        char *result;
        if (n1 == 0 || (right != NULL && n2 == 0)) {
            result = strdup("[Process status]\nInvalid job: missing command\n");
        } else {
            result = run_job(args1, right ? args2 : NULL);
            if (result == NULL)
                result = strdup("[Process status]\nJob could not be started\n");
        }

        if (result != NULL) {
            writeOutput(original, result);    /* only the Parent writes the file */
            free(result);
        }
        free(original);
    }

    free(workload);
    printf("All jobs executed. Results written to output.txt\n");
    return EXIT_SUCCESS;
}
