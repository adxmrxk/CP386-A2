//AdamRak
//WilfridLaurierUniversity
//210700280
//rakx0280@mylaurier.ca

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define ASSIGNMENTS_PER_CHAPTER 2

typedef struct {
    int assignment;   
    double average;
} Result;

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

static int assignments_in_chapter(int cols, int chapter) {
    int left = cols - chapter * ASSIGNMENTS_PER_CHAPTER;
    return left < ASSIGNMENTS_PER_CHAPTER ? left : ASSIGNMENTS_PER_CHAPTER;
}


static double **read_grades(const char *filename, int *rows, int *cols) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Failed to access grades file '%s': %s\n", filename, strerror(errno));
        return NULL;
    }

    char line[4096];
    *cols = 0;
    while (*cols == 0 && fgets(line, sizeof line, fp) != NULL) {
        for (char *tok = strtok(line, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n"))
            (*cols)++;
    }
    rewind(fp);

    if (*cols == 0) {
        printf("Error: grades file '%s' is empty\n", filename);
        fclose(fp);
        return NULL;
    }

    double **grades = NULL;
    int count = 0;
    int capacity = 0;
    for (;;) {
        double *row = malloc(*cols * sizeof *row);
        if (row == NULL) die("malloc");

        int j;
        for (j = 0; j < *cols; j++) {
            if (fscanf(fp, "%lf", &row[j]) != 1) break;
        }
        if (j < *cols) {   /* end of file, or an incomplete row */
            free(row);
            break;
        }

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            double **bigger = realloc(grades, capacity * sizeof *bigger);
            if (bigger == NULL) die("realloc");
            grades = bigger;
        }
        grades[count++] = row;
    }
    fclose(fp);

    *rows = count;
    return grades;
}

static void ta_process(int column, double **grades, int rows, int out_fd) {
    double sum_1 = 0;
    for (int i = 0; i < rows; i++)
        sum_1 += grades[i][column];

    Result r = { column + 1, sum_1 / rows };
    write_all(out_fd, &r, sizeof r);
    close(out_fd);
    _exit(EXIT_SUCCESS);
}

static void gradta_process(int chapter, int assignments, double **grades, int rows, int out_fd) {
    int pipes[ASSIGNMENTS_PER_CHAPTER][2];
    pid_t pids[ASSIGNMENTS_PER_CHAPTER];

    for (int k = 0; k < assignments; k++) {
        if (pipe(pipes[k]) == -1) die("pipe");

        pids[k] = fork();
        if (pids[k] == -1) die("fork");

        if (pids[k] == 0) {
            close(pipes[k][0]);
            ta_process(chapter * ASSIGNMENTS_PER_CHAPTER + k, grades, rows, pipes[k][1]);
        }
        close(pipes[k][1]);
    }

    for (int k = 0; k < assignments; k++) {
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
        printf("Usage: ./assignment_average [sample_in_grades.txt]\n");
        return EXIT_FAILURE;
    }

    int rows, cols;
    double **grades = read_grades(argv[1], &rows, &cols);
    if (grades == NULL) {
        return EXIT_SUCCESS;
    }
    if (rows == 0) {
        printf("Error: no grade rows were found in '%s'\n", argv[1]);
        return EXIT_SUCCESS;
    }

    int chapters = (cols + ASSIGNMENTS_PER_CHAPTER - 1) / ASSIGNMENTS_PER_CHAPTER;
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
            gradta_process(c, assignments_in_chapter(cols, c), grades, rows, pipes[c][1]);
        }
        close(pipes[c][1]);
    }

    for (int c = 0; c < chapters; c++) {
        int assignments = assignments_in_chapter(cols, c);
        for (int k = 0; k < assignments; k++) {
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
