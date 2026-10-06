#define _GNU_SOURCE
#include "prun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

static long long timespec_diff_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000LL + (end.tv_nsec - start.tv_nsec) / 1000000LL;
}

char **tokenize_command(const char *cmd_line) {
    if (!cmd_line) return NULL;
    char **argv = malloc(sizeof(char *) * 64);
    int count = 0;
    const char *p = cmd_line;

    while (*p && count < 63) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        char buf[1024];
        int bi = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                while (*p && *p != quote && bi < 1023) {
                    buf[bi++] = *p++;
                }
                if (*p == quote) p++;
            } else {
                if (bi < 1023) buf[bi++] = *p++;
                else p++;
            }
        }
        buf[bi] = '\0';
        argv[count++] = strdup(buf);
    }
    argv[count] = NULL;
    return argv;
}

void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
    }
    free(argv);
}

/**
 * run_parallel:
 * The core process dispatcher engine.
 */
int run_parallel(task_t *tasks, int num_tasks, int max_workers, run_summary_t *summary, bool verbose) {
    if (!tasks || num_tasks <= 0 || max_workers <= 0) {
        return -1;
    }

    int active_workers = 0;
    int next_task_idx = 0;
    int completed_count = 0;
    struct timespec wall_start, wall_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);

    while (completed_count < num_tasks) {
        // 1. Dispatch tasks while slots are available
        while (active_workers < max_workers && next_task_idx < num_tasks) {
            task_t *t = &tasks[next_task_idx];
            clock_gettime(CLOCK_MONOTONIC, &t->start);

            pid_t pid = fork();
            if (pid < 0) {
                t->failed = true;
                t->finished = true;
                completed_count++;
                next_task_idx++;
                continue;
            } else if (pid == 0) {
                // Child: replace process image
                execvp(t->argv[0], t->argv);
                _exit(127); // If execvp fails (e.g. command not found)
            } else {
                // Parent: record child PID and occupied slot
                t->pid = pid;
                active_workers++;
                if (verbose) {
                    printf("  [START] [PID %d] %s (Slot %d/%d)\n", pid, t->cmd_line, active_workers, max_workers);
                    fflush(stdout);
                }
                next_task_idx++;
            }
        }

        // 2. Wait for ANY child process to finish
        if (active_workers > 0) {
            int status;
            pid_t completed_pid = waitpid(-1, &status, 0);

            if (completed_pid > 0) {
                struct timespec end_time;
                clock_gettime(CLOCK_MONOTONIC, &end_time);

                // Find which task matched this completed PID
                for (int i = 0; i < next_task_idx; i++) {
                    if (tasks[i].pid == completed_pid && !tasks[i].finished) {
                        tasks[i].finished = true;
                        tasks[i].elapsed_ms = timespec_diff_ms(tasks[i].start, end_time);

                        // Extract exit code or termination signal
                        if (WIFEXITED(status)) {
                            tasks[i].exit_status = WEXITSTATUS(status);
                        } else if (WIFSIGNALED(status)) {
                            tasks[i].exit_status = 128 + WTERMSIG(status);
                        } else {
                            tasks[i].exit_status = status;
                        }

                        tasks[i].failed = (tasks[i].exit_status != 0);

                        // Free the worker slot!
                        active_workers--;
                        completed_count++;

                        if (verbose) {
                            printf("  [%s] [PID %d] \"%s\" in %lldms (exit %d)\n",
                                   tasks[i].failed ? "FAIL " : "DONE ",
                                   completed_pid, tasks[i].cmd_line, tasks[i].elapsed_ms, tasks[i].exit_status);
                            fflush(stdout);
                        }

                        break; // Found the task, stop searching
                    }
                }
            }
        }
    }

    // 3. Record final wall-clock duration and summary
    clock_gettime(CLOCK_MONOTONIC, &wall_end);

    if (summary) {
        summary->total_tasks = num_tasks;
        summary->succeeded_tasks = 0;
        summary->failed_tasks = 0;
        long long sum_task_time = 0;

        for (int i = 0; i < num_tasks; i++) {
            if (tasks[i].failed) {
                summary->failed_tasks++;
            } else {
                summary->succeeded_tasks++;
            }
            sum_task_time += tasks[i].elapsed_ms;
        }

        summary->wall_clock_ms = timespec_diff_ms(wall_start, wall_end);
        if (summary->wall_clock_ms > 0) {
            summary->speedup = (double)sum_task_time / (double)summary->wall_clock_ms;
        } else {
            summary->speedup = 1.0;
        }
    }

    return 0;
}

#ifndef TEST_BUILD
int main(int argc, char **argv) {
    int max_workers = 4;
    char *filename = NULL;

    // Parse options: -j <workers> <file>
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            max_workers = atoi(argv[++i]);
            if (max_workers <= 0) max_workers = 1;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    if (!filename) {
        printf("Usage: %s [-j workers] <task_file.txt>\n", argv[0]);
        printf("Example: %s -j 4 sample_tasks.txt\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Error opening task file");
        return 1;
    }

    char line[1024];
    task_t tasks[256];
    int num_tasks = 0;

    while (fgets(line, sizeof(line), f) && num_tasks < 256) {
        // Strip trailing newline
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0 || line[0] == '#') continue; // Skip comments and empty lines

        tasks[num_tasks].cmd_line = strdup(line);
        tasks[num_tasks].argv = tokenize_command(line);
        tasks[num_tasks].pid = -1;
        tasks[num_tasks].finished = false;
        tasks[num_tasks].failed = false;
        tasks[num_tasks].elapsed_ms = 0;
        tasks[num_tasks].exit_status = -1;
        num_tasks++;
    }
    fclose(f);

    printf("============================================================\n");
    printf("  prun: Dispatching %d tasks with %d concurrent workers\n", num_tasks, max_workers);
    printf("============================================================\n\n");

    run_summary_t summary;
    int ret = run_parallel(tasks, num_tasks, max_workers, &summary, true);

    if (ret < 0) {
        printf("\n[ERROR] run_parallel failed!\n");
        return 1;
    }

    printf("\n============================================================\n");
    printf("  Execution Summary\n");
    printf("============================================================\n");
    printf("  Total Tasks:    %d\n", summary.total_tasks);
    printf("  Succeeded:      %d\n", summary.succeeded_tasks);
    printf("  Failed:         %d\n", summary.failed_tasks);
    printf("  Wall Clock:     %lld ms\n", summary.wall_clock_ms);
    printf("  Est. Speedup:   %.2fx\n", summary.speedup);
    printf("============================================================\n");

    for (int i = 0; i < num_tasks; i++) {
        free(tasks[i].cmd_line);
        free_argv(tasks[i].argv);
    }

    return (summary.failed_tasks == 0) ? 0 : 1;
}
#endif
