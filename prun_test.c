// This file is totally vibe coded oh sorry.. vibed copy pasted
#define _GNU_SOURCE
#ifndef TEST_BUILD
#define TEST_BUILD
#endif
#include "prun.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  [\033[31mFAIL\033[0m] %s (Line %d)\n", msg, __LINE__); \
    } else { \
        printf("  [\033[32mPASS\033[0m] %s\n", msg); \
        tests_passed++; \
    } \
} while (0)

static void timeout_handler(int sig) {
    (void)sig;
    printf("\n  [\033[31mTIMEOUT / DEADLOCK\033[0m] prun test timed out!\n");
    printf("  Check your waitpid loop or process reaping logic.\n");
    exit(1);
}

void test_invalid_arguments() {
    printf("[1/5] Testing argument validation...\n");
    task_t dummy;
    run_summary_t sum;
    TEST_ASSERT(run_parallel(NULL, 1, 2, &sum, false) == -1, "run_parallel rejects NULL tasks");
    TEST_ASSERT(run_parallel(&dummy, 0, 2, &sum, false) == -1, "run_parallel rejects 0 tasks");
    TEST_ASSERT(run_parallel(&dummy, 1, 0, &sum, false) == -1, "run_parallel rejects 0 workers");
    TEST_ASSERT(run_parallel(&dummy, 1, -2, &sum, false) == -1, "run_parallel rejects negative workers");
}

void test_single_task() {
    printf("[2/5] Testing single task execution...\n");
    task_t t;
    t.cmd_line = "true";
    t.argv = tokenize_command("true");
    t.finished = false;
    t.failed = false;
    t.exit_status = -1;

    run_summary_t sum;
    alarm(3);
    int res = run_parallel(&t, 1, 1, &sum, false);
    alarm(0);

    TEST_ASSERT(res == 0, "run_parallel returned 0");
    TEST_ASSERT(t.finished == true, "Task marked finished");
    TEST_ASSERT(t.exit_status == 0, "Task exit status is 0");
    TEST_ASSERT(sum.succeeded_tasks == 1, "Summary records 1 succeeded task");
    free_argv(t.argv);
}

void test_exit_codes() {
    printf("[3/5] Testing exit code extraction & error reporting...\n");
    char *cmd0[] = {"true", NULL};
    char *cmd1[] = {"sh", "-c", "exit 42", NULL};
    char *cmd2[] = {"/nonexistent_prun_command_xyz", NULL};

    task_t tasks[3];
    tasks[0].cmd_line = "true";
    tasks[0].argv = cmd0;
    tasks[0].finished = false;
    tasks[0].failed = false;

    tasks[1].cmd_line = "exit 42";
    tasks[1].argv = cmd1;
    tasks[1].finished = false;
    tasks[1].failed = false;

    tasks[2].cmd_line = "bad_cmd";
    tasks[2].argv = cmd2;
    tasks[2].finished = false;
    tasks[2].failed = false;

    run_summary_t sum;
    alarm(4);
    int res = run_parallel(tasks, 3, 3, &sum, false);
    alarm(0);

    TEST_ASSERT(res == 0, "run_parallel executed all 3 tasks");
    TEST_ASSERT(tasks[0].exit_status == 0 && tasks[0].failed == false, "Task 0 exited 0 (succeeded)");
    TEST_ASSERT(tasks[1].exit_status == 42 && tasks[1].failed == true, "Task 1 exited 42 (failed)");
    TEST_ASSERT(tasks[2].exit_status == 127 && tasks[2].failed == true, "Task 2 exited 127 (exec failure)");
    TEST_ASSERT(sum.succeeded_tasks == 1 && sum.failed_tasks == 2, "Summary reports 1 success, 2 failures");
}

void test_parallel_speedup() {
    printf("[4/5] Testing parallel execution (4 workers, 4x60ms tasks)...\n");
    task_t tasks[4];
    for (int i = 0; i < 4; i++) {
        tasks[i].cmd_line = "sleep 0.06";
        tasks[i].argv = tokenize_command("sleep 0.06");
        tasks[i].finished = false;
        tasks[i].failed = false;
    }

    run_summary_t sum;
    alarm(4);
    int res = run_parallel(tasks, 4, 4, &sum, false);
    alarm(0);

    TEST_ASSERT(res == 0, "Parallel run succeeded");
    TEST_ASSERT(sum.succeeded_tasks == 4, "All 4 sleep tasks succeeded");
    // Sequential would take 240ms. Parallel on 4 cores takes ~60-150ms.
    TEST_ASSERT(sum.wall_clock_ms < 200, "Tasks executed concurrently (wall_clock_ms < 200ms)");
    TEST_ASSERT(sum.speedup > 1.5, "Parallel speedup achieved (> 1.5x)");

    for (int i = 0; i < 4; i++) free_argv(tasks[i].argv);
}

void test_bounded_concurrency() {
    printf("[5/5] Testing worker throttling (max 2 workers, 4x50ms tasks)...\n");
    // 4 tasks of 50ms throttled to 2 workers must run in 2 batches, taking >= 90ms
    task_t tasks[4];
    for (int i = 0; i < 4; i++) {
        tasks[i].cmd_line = "sleep 0.05";
        tasks[i].argv = tokenize_command("sleep 0.05");
        tasks[i].finished = false;
        tasks[i].failed = false;
    }

    run_summary_t sum;
    alarm(4);
    int res = run_parallel(tasks, 4, 2, &sum, false);
    alarm(0);

    TEST_ASSERT(res == 0, "Throttled run succeeded");
    TEST_ASSERT(sum.wall_clock_ms >= 85, "Enforced worker limit (2 waves took >= 85ms)");

    for (int i = 0; i < 4; i++) free_argv(tasks[i].argv);
}

int main() {
    printf("=====================================================\n");
    printf("  prun Test Suite: Parallel Task Dispatcher\n");
    printf("=====================================================\n\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = timeout_handler;
    sigaction(SIGALRM, &sa, NULL);

    test_invalid_arguments();
    test_single_task();
    test_exit_codes();
    test_parallel_speedup();
    test_bounded_concurrency();

    printf("\n-----------------------------------------------------\n");
    printf("Summary: %d / %d tests passed.\n", tests_passed, tests_total);
    printf("-----------------------------------------------------\n");

    return (tests_passed == tests_total) ? 0 : 1;
}
