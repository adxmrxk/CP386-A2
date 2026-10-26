CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g

all: grades

grades: assignment_average.c
	$(CC) $(CFLAGS) -o grades assignment_average.c

runq1: grades
	./grades sample_in_grades.txt

clean:
	rm -f grades

.PHONY: all runq1 clean
