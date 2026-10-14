/* The device benchmark's journal (benchmark_plan.h): a text file that only
   grows, one line per event, each written with one write() and synced.

     visr benchmark journal 1
     suite full K7Q2M9XD 2160 1080 16 512 1 5f3a9c01
     begin 0 A menu 1080 16 512 max
     memory 512 904
     end 0 ok 16.9 4.1 7.3 59.94 904
     crash 11
     done

   A line with no newline at the end is a write the system cut short, and is ignored. */
#include "benchmark_plan.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_HEADER "visr benchmark journal 1"

static int append_line(const char *path, int flags, const char *format, ...) __attribute__((format(printf, 3, 4)));
static int append_line(const char *path, int flags, const char *format, ...) {
    char line[256];
    va_list arguments;
    int length, descriptor, written;

    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    if (length < 0 || length >= (int)sizeof(line) - 1) return 0;
    line[length++] = '\n';
    descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | flags, 0644);
    if (descriptor < 0) return 0;
    written = (int)write(descriptor, line, (size_t)length);
    fsync(descriptor);
    close(descriptor);
    return written == length;
}

int benchmark_journal_start(const char *path, enum benchmark_mode mode, const char *id, const struct benchmark_device *device,
    unsigned long signature) {
    return append_line(path, O_TRUNC, "%s", JOURNAL_HEADER) &&
        append_line(path, 0, "suite %s %s %ld %ld %ld %ld %d %08lx", benchmark_mode_name(mode), id, device->drawable_lines,
            device->base_lines, device->anisotropy, device->shadow_size, device->model_lod_max, signature);
}

int benchmark_journal_begin(const char *path, int index, const char *key) {
    return append_line(path, 0, "begin %d %s", index, key);
}

int benchmark_journal_end(const char *path, int index, const struct benchmark_outcome *outcome) {
    return append_line(path, 0, "end %d %s %.2f %.2f %.2f %.3f %ld", index, benchmark_status_name(outcome->status),
        outcome->interval_p99, outcome->cpu_p95, outcome->gpu_p95, outcome->refresh_hertz, outcome->left_least);
}

int benchmark_journal_memory(const char *path, long footprint, long left) {
    return append_line(path, 0, "memory %ld %ld", footprint, left);
}

int benchmark_journal_done(const char *path) {
    return append_line(path, 0, "done");
}

int benchmark_journal_marker_open(const char *path) {
    return open(path, O_WRONLY | O_APPEND);
}

/* only write(): this runs in a signal handler */
void benchmark_journal_mark_crash(int descriptor, int signal_number) {
    char line[16] = "crash ";
    int length = 6;

    if (descriptor < 0) return;
    if (signal_number < 0) signal_number = 0;
    if (signal_number >= 100) line[length++] = (char)('0' + signal_number / 100 % 10);
    if (signal_number >= 10) line[length++] = (char)('0' + signal_number / 10 % 10);
    line[length++] = (char)('0' + signal_number % 10);
    line[length++] = '\n';
    (void)!write(descriptor, line, (size_t)length);
}

static enum benchmark_status status_named(const char *name) {
    int status;

    for (status = 0; status < BENCHMARK_STATUS_COUNT; status++)
        if (!strcmp(name, benchmark_status_name((enum benchmark_status)status))) return (enum benchmark_status)status;
    return BENCHMARK_FAILED;
}

void benchmark_journal_load(const char *path, struct benchmark_journal *journal) {
    FILE *file = fopen(path, "r");
    char line[256];
    int open_run = -1, header = 0;

    memset(journal, 0, sizeof(*journal));
    journal->last_footprint = journal->last_left = -1;
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        char mode[16], id[16], status[16];
        struct benchmark_outcome outcome = { 0 };
        int index, number;

        if (!length || line[length - 1] != '\n') break;   /* a write cut short */
        line[length - 1] = 0;
        if (!header) {
            if (strcmp(line, JOURNAL_HEADER)) break;
            header = 1;
        } else if (sscanf(line, "suite %15s %15s %ld %ld %ld %ld %d %lx", mode, id, &journal->device.drawable_lines,
            &journal->device.base_lines, &journal->device.anisotropy, &journal->device.shadow_size,
            &journal->device.model_lod_max, &journal->signature) == 8) {
            journal->valid = 1;
            journal->mode = !strcmp(mode, "full") ? BENCHMARK_FULL : BENCHMARK_QUICK;
            snprintf(journal->id, sizeof(journal->id), "%s", id);
        } else if (!journal->valid) {
            break;
        } else if (sscanf(line, "begin %d", &index) == 1) {
            /* a begin for a run that is not the next one means a journal that was edited or mixed up: stop trusting it */
            if (index != journal->count || open_run >= 0 || index >= BENCHMARK_RUNS_MAXIMUM) { journal->valid = 0; break; }
            open_run = index;
            journal->crash_signal = 0;
            journal->last_footprint = journal->last_left = -1;
        } else if (sscanf(line, "end %d %15s %lf %lf %lf %lf %ld", &index, status, &outcome.interval_p99, &outcome.cpu_p95,
            &outcome.gpu_p95, &outcome.refresh_hertz, &outcome.left_least) == 7) {
            if (index != open_run) { journal->valid = 0; break; }
            outcome.status = status_named(status);
            journal->outcomes[journal->count++] = outcome;
            open_run = -1;
        } else if (sscanf(line, "memory %ld %ld", &journal->last_footprint, &journal->last_left) == 2) {
        } else if (sscanf(line, "crash %d", &number) == 1) {
            journal->crash_signal = number;
        } else if (!strcmp(line, "done")) {
            journal->finished = 1;
        }
    }
    fclose(file);
    if (journal->valid && open_run >= 0) {
        /* the run that began and never ended: a signal's mark says it crashed; with none, the system ended the app */
        struct benchmark_outcome *cut = &journal->outcomes[journal->count++];

        memset(cut, 0, sizeof(*cut));
        cut->status = journal->crash_signal ? BENCHMARK_CRASHED : BENCHMARK_KILLED;
        cut->left_least = journal->last_left;
        journal->cut_off = 1;
    }
}
