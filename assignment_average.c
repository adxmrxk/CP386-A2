/*
 * CP386 Assignment 2 - Question 1
 *
 * Process tree:
 *   Teacher -> GradTA (one per chapter) -> TA (one per assignment)
 *
 * Each TA averages one assignment column and sends the result back up the
 * tree through pipes. The Teacher prints the results in assignment order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define ASSIGNMENTS_PER_CHAPTER 2

/* One TA's result, sent through a pipe */
typedef struct {
    int assignment;   /* 1-based assignment number */
    double average;
} Result;

/* Used in every process. _exit skips stdio cleanup, which is safe in forked children. */
static void die(const char *msg) {
    perror(msg);
    _exit(EXIT_FAILURE);
}

static void write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n == -1) {
            if (errno == EINTR) continue;
            die("write");
        }
        p += n;
        len -= (size_t)n;
    }
}

static void read_exact(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == -1) {
            if (errno == EINTR) continue;
            die("read");
        }
        if (n == 0) {
            fprintf(stderr, "unexpected end of pipe\n");
            _exit(EXIT_FAILURE);
        }
        p += n;
        len -= (size_t)n;
    }
}

/*
 * Reads the grades file into a dynamically allocated 2-D array.
 * The number of columns comes from the first line, and the number of rows
 * is however many complete rows follow.
 */
static int **read_grades(const char *filename, int *rows, int *cols) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        perror(filename);
        return NULL;
    }

    char line[4096];
    if (fgets(line, sizeof line, fp) == NULL) {
        fprintf(stderr, "%s is empty\n", filename);
        fclose(fp);
        return NULL;
    }

    *cols = 0;
    for (char *tok = strtok(line, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n"))
        (*cols)++;
    rewind(fp);

    if (*cols == 0 || *cols % ASSIGNMENTS_PER_CHAPTER != 0) {
        fprintf(stderr, "Expected a multiple of %d columns, found %d\n",
                ASSIGNMENTS_PER_CHAPTER, *cols);
        fclose(fp);
        return NULL;
    }

    int **grades = NULL;
    int count = 0;
    int capacity = 0;
    for (;;) {
        int *row = malloc(*cols * sizeof *row);
        if (row == NULL) die("malloc");

        int j;
        for (j = 0; j < *cols; j++) {
            if (fscanf(fp, "%d", &row[j]) != 1) break;
        }
        if (j < *cols) {   /* end of file, or an incomplete row */
            free(row);
            break;
        }

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            int **bigger = realloc(grades, capacity * sizeof *bigger);
            if (bigger == NULL) die("realloc");
            grades = bigger;
        }
        grades[count++] = row;
    }
    fclose(fp);

    *rows = count;
    return grades;
}

/* TA: averages one assignment column and sends the result to its GradTA */
static void ta_process(int column, int **grades, int rows, int out_fd) {
    double sum = 0;
    for (int i = 0; i < rows; i++)
        sum += grades[i][column];

    Result r = { column + 1, sum / rows };
    write_all(out_fd, &r, sizeof r);
    close(out_fd);
    _exit(EXIT_SUCCESS);
}

/* GradTA: owns one chapter and creates one TA per assignment in it */
static void gradta_process(int chapter, int **grades, int rows, int out_fd) {
    int pipes[ASSIGNMENTS_PER_CHAPTER][2];
    pid_t pids[ASSIGNMENTS_PER_CHAPTER];

    for (int k = 0; k < ASSIGNMENTS_PER_CHAPTER; k++) {
        if (pipe(pipes[k]) == -1) die("pipe");

        pids[k] = fork();
        if (pids[k] == -1) die("fork");

        if (pids[k] == 0) {
            close(pipes[k][0]);
            ta_process(chapter * ASSIGNMENTS_PER_CHAPTER + k, grades, rows, pipes[k][1]);
        }
        close(pipes[k][1]);
    }

    /* Pass each TA's result up to the Teacher */
    for (int k = 0; k < ASSIGNMENTS_PER_CHAPTER; k++) {
        Result r;
        read_exact(pipes[k][0], &r, sizeof r);
        close(pipes[k][0]);
        waitpid(pids[k], NULL, 0);
        write_all(out_fd, &r, sizeof r);
    }

    close(out_fd);
    _exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <grades file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int rows, cols;
    int **grades = read_grades(argv[1], &rows, &cols);
    if (grades == NULL || rows == 0) {
        fprintf(stderr, "No grades were read from %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* The number of chapters comes from the number of columns, so the program adapts to the file */
    int chapters = cols / ASSIGNMENTS_PER_CHAPTER;
    int (*pipes)[2] = malloc(chapters * sizeof *pipes);
    pid_t *pids = malloc(chapters * sizeof *pids);
    if (pipes == NULL || pids == NULL) die("malloc");

    /* Teacher creates one GradTA per chapter */
    for (int c = 0; c < chapters; c++) {
        if (pipe(pipes[c]) == -1) die("pipe");

        pids[c] = fork();
        if (pids[c] == -1) die("fork");

        if (pids[c] == 0) {
            close(pipes[c][0]);
            gradta_process(c, grades, rows, pipes[c][1]);
        }
        close(pipes[c][1]);
    }

    /* Read chapter by chapter so the output is in assignment order */
    for (int c = 0; c < chapters; c++) {
        for (int k = 0; k < ASSIGNMENTS_PER_CHAPTER; k++) {
            Result r;
            read_exact(pipes[c][0], &r, sizeof r);
            printf("Assignment %d - Average = %f\n", r.assignment, r.average);
        }
        close(pipes[c][0]);
        waitpid(pids[c], NULL, 0);
    }

    for (int i = 0; i < rows; i++)
        free(grades[i]);
    free(grades);
    free(pipes);
    free(pids);

    return EXIT_SUCCESS;
}
