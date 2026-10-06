#ifndef PRUN_H
#define PRUN_H

#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

/**
 * Represents a single task to be executed.
 */
typedef struct {
    char *cmd_line;         // Raw command string (e.g. "sha256sum /etc/hosts")
    char **argv;            // NULL-terminated array of arguments for execvp()
    pid_t pid;              // PID of child process when running
    struct timespec start;  // Timestamp when spawned
    long long elapsed_ms;   // Execution duration in milliseconds
    int exit_status;        // Exit code from waitpid()
    bool finished;          // True once process has terminated and been reaped
    bool failed;            // True if exit_status != 0 or killed by signal
} task_t;

/**
 * Summary metrics of a parallel run batch.
 */
typedef struct {
    int total_tasks;
    int succeeded_tasks;
    int failed_tasks;
    long long wall_clock_ms;
    double speedup;         // (Sum of task times) / (Total wall clock time)
} run_summary_t;

/**
 * Executes an array of tasks in parallel, limiting active concurrent processes
 * to at most `max_workers`.
 *
 * Parameters:
 *  - tasks: Array of task_t structs.
 *  - num_tasks: Number of tasks in the array (must be > 0).
 *  - max_workers: Maximum number of active child processes (must be > 0).
 *  - summary: Pointer to run_summary_t struct to store performance metrics.
 *  - verbose: If true, prints live [START] and [DONE] messages to stdout.
 *
 * Requirements:
 *  1. At no point should there be more than `max_workers` child processes running.
 *  2. While active_workers < max_workers and tasks remain:
 *     - Record start time with clock_gettime(CLOCK_MONOTONIC, &task->start).
 *     - fork() a child process.
 *     - In child: call execvp(task->argv[0], task->argv). If execvp fails, exit with 127.
 *     - In parent: save task->pid and increment active_workers.
 *  3. Use waitpid(-1, &status, 0) to wait for any child process to complete.
 *  4. Identify the completed task, record end time, calculate elapsed_ms.
 *  5. Extract exit code (WIFEXITED / WEXITSTATUS or WIFSIGNALED).
 *  6. Decrement active_workers and immediately dispatch the next available task.
 *  7. Populate summary with total, succeeded, failed, wall_clock_ms, and speedup.
 *
 * Returns 0 on success, or -1 on invalid arguments.
 */
int run_parallel(task_t *tasks, int num_tasks, int max_workers, run_summary_t *summary, bool verbose);

/**
 * Helper to split a command string by whitespace into a NULL-terminated argv array.
 */
char **tokenize_command(const char *cmd_line);

/**
 * Helper to free an argv array created by tokenize_command.
 */
void free_argv(char **argv);

#endif // PRUN_H
