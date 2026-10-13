/*
 * mcron - minimalist cron, see mcron(1)
 *
 * Missed runs are detected from a single persistent "last processed minute"
 * (STATE_DIR/last_tick). After a gap of up to MAX_CATCHUP every job whose
 * schedule fell into the gap runs once; a minute is never processed twice.
 */

#define _DEFAULT_SOURCE

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_FILE
#define CONFIG_FILE "/etc/mcron.conf"
#endif
#ifndef STATE_DIR
#define STATE_DIR "/var/lib/mcron"
#endif
#ifndef VERSION
#define VERSION "unknown"
#endif

#define TICK_FILE "last_tick"
#define TICK_TMP "last_tick.tmp"
#define PID_FILE "mcron.pid"

#define MAX_LINE 256 /* keep in sync with the %255[^\n] width below */
#define TOKEN_SIZE 16 /* keep in sync with the %15s widths below */
#define NUMBER_BUF_SIZE 32
#define DECIMAL_BASE 10
#define DIR_MODE 0755
#define FILE_MODE 0644
#define DAEMON_UMASK 022
#define EXIT_CANNOT_EXEC 127
#define SUNDAY_ALIAS 7
#define SECONDS_PER_MINUTE 60L
#define SECONDS_PER_DAY (24L * 60L * SECONDS_PER_MINUTE)
#define MAX_CATCHUP (32L * SECONDS_PER_DAY)
#define ANY (-1)
#define NO_FD (-1)

enum { FIELD_MIN, FIELD_HOUR, FIELD_DOM, FIELD_MON, FIELD_DOW, FIELD_COUNT };
enum { LINE_OK, LINE_TOO_LONG, LINE_EOF };

static const int field_low[FIELD_COUNT] = { 0, 0, 1, 1, 0 };
static const int field_high[FIELD_COUNT] = { 59, 23, 31, 12, 7 };

typedef struct {
	int dir; /* fd of STATE_DIR; all state files go through it */
	int pid; /* fd of the locked pid file, held until exit */
	int report; /* config changed since last pass: log its problems */
	time_t seen_mtime; /* config mtime of the previous pass */
} State;

typedef struct {
	time_t first; /* first minute to process */
	time_t last; /* last minute to process (inclusive) */
} Window;

typedef struct {
	int when[FIELD_COUNT];
	char command[MAX_LINE];
} Job;

static void
die(const char *message)
{
	fprintf(stderr, "mcron: %s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

/* owned by the user we run as and not writable by group or others */
static int
trusted(const struct stat *info)
{
	return info->st_uid == geteuid() &&
	       !(info->st_mode & (S_IWGRP | S_IWOTH));
}

static time_t
minute_floor(time_t when)
{
	return when - (when % SECONDS_PER_MINUTE);
}

static void
open_statedir(State *state)
{
	struct stat info;

	mkdir(STATE_DIR, DIR_MODE); /* EEXIST is fine, others fail below */
	state->dir = open(STATE_DIR,
			  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (state->dir < 0)
		die("cannot open " STATE_DIR);
	if (fstat(state->dir, &info) < 0)
		die("cannot stat " STATE_DIR);
	if (!S_ISDIR(info.st_mode) || !trusted(&info)) {
		fputs("mcron: " STATE_DIR " must be owned by us and not "
		      "group/world writable\n",
		      stderr);
		exit(EXIT_FAILURE);
	}
}

static void
lock_instance(State *state)
{
	/* no O_TRUNC: must not clobber the pid of a running instance */
	state->pid = openat(state->dir, PID_FILE,
			    O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
			    FILE_MODE);
	if (state->pid < 0)
		die("cannot open pid file");
	if (flock(state->pid, LOCK_EX | LOCK_NB) < 0)
		die("cannot lock pid file (already running?)");
}

static void
write_pid(const State *state)
{
	if (ftruncate(state->pid, 0) == 0)
		dprintf(state->pid, "%ld\n", (long)getpid());
}

static void
daemonize(void)
{
	pid_t pid = fork();

	if (pid < 0)
		die("fork");
	if (pid > 0)
		exit(EXIT_SUCCESS);

	if (setsid() < 0)
		die("setsid");
	signal(SIGHUP, SIG_IGN); /* reset to SIG_DFL in children, see spawn() */

	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid > 0)
		exit(EXIT_SUCCESS);

	umask(DAEMON_UMASK);
	if (chdir("/") < 0)
		die("chdir");
	if (!freopen("/dev/null", "r", stdin) ||
	    !freopen("/dev/null", "w", stdout) ||
	    !freopen("/dev/null", "w", stderr))
		exit(EXIT_FAILURE);
}

/* last processed minute, or 0 if unknown */
static time_t
read_tick(const State *state)
{
	char buf[NUMBER_BUF_SIZE];
	char *end;
	long long value;
	ssize_t count;
	int desc;

	desc = openat(state->dir, TICK_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (desc < 0)
		return 0;
	count = read(desc, buf, sizeof(buf) - 1);
	close(desc);
	if (count <= 0)
		return 0;
	buf[count] = '\0';

	errno = 0;
	value = strtoll(buf, &end, DECIMAL_BASE);
	if (errno != 0 || end == buf || value < 0)
		return 0;
	return (time_t)value;
}

/* atomic: write a temp file, then rename it over the old one */
static void
write_tick(const State *state, time_t tick)
{
	char buf[NUMBER_BUF_SIZE];
	int count;
	int desc;

	count = snprintf(buf, sizeof(buf), "%lld\n", (long long)tick);
	desc = openat(state->dir, TICK_TMP,
		      O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
		      FILE_MODE);
	if (desc < 0)
		goto fail;
	if (write(desc, buf, (size_t)count) != count) {
		close(desc);
		goto fail;
	}
	if (close(desc) < 0)
		goto fail;
	if (renameat(state->dir, TICK_TMP, state->dir, TICK_FILE) < 0)
		goto fail;
	return;
fail:
	syslog(LOG_ERR, "cannot write %s: %s", TICK_FILE, strerror(errno));
}

/* "*" or a number within the limits of field `idx`; 0 on success */
static int
parse_field(const char *text, size_t idx, int *out)
{
	char *end;
	long value;

	if (strcmp(text, "*") == 0) {
		*out = ANY;
		return 0;
	}
	errno = 0;
	value = strtol(text, &end, DECIMAL_BASE);
	if (errno != 0 || end == text || *end != '\0' ||
	    value < field_low[idx] || value > field_high[idx])
		return -1;
	*out = (int)value;
	return 0;
}

/* returns 0 on success, -1 if the line is malformed */
static int
parse_job(const State *state, const char *line, Job *job)
{
	char tok[FIELD_COUNT][TOKEN_SIZE];
	size_t idx;

	if (sscanf(line, "%15s %15s %15s %15s %15s %255[^\n]", tok[FIELD_MIN],
		   tok[FIELD_HOUR], tok[FIELD_DOM], tok[FIELD_MON],
		   tok[FIELD_DOW], job->command) != FIELD_COUNT + 1) {
		if (state->report)
			syslog(LOG_ERR, "config: malformed line: %s", line);
		return -1;
	}
	for (idx = 0; idx < FIELD_COUNT; idx++) {
		if (parse_field(tok[idx], idx, &job->when[idx]) < 0) {
			if (state->report)
				syslog(LOG_ERR,
				       "config: bad field '%s' in "
				       "line: %s",
				       tok[idx], line);
			return -1;
		}
	}
	if (job->when[FIELD_DOW] == SUNDAY_ALIAS)
		job->when[FIELD_DOW] = 0;
	return 0;
}

static int
schedule_matches(const Job *job, const struct tm *local)
{
	const int current[FIELD_COUNT] = { local->tm_min, local->tm_hour,
					   local->tm_mday, local->tm_mon + 1,
					   local->tm_wday };
	size_t idx;

	for (idx = 0; idx < FIELD_COUNT; idx++) {
		if (job->when[idx] != ANY && job->when[idx] != current[idx])
			return 0;
	}
	return 1;
}

static void
spawn(const char *command)
{
	pid_t pid = fork();

	if (pid < 0) {
		syslog(LOG_ERR, "fork failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		/* don't leak our signal dispositions through exec */
		signal(SIGHUP, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(EXIT_CANNOT_EXEC);
	}
	syslog(LOG_INFO, "run: %s", command);
}

/*
 * Run the job if its schedule matches any minute of the window.
 * At most one run per call: catching up N missed minutes costs one run.
 */
static void
process_job(const State *state, const Window *window, const char *line)
{
	Job job;
	struct tm local;
	time_t minute;

	if (parse_job(state, line, &job) < 0)
		return;
	for (minute = window->first; minute <= window->last;
	     minute += SECONDS_PER_MINUTE) {
		localtime_r(&minute, &local);
		if (schedule_matches(&job, &local)) {
			spawn(job.command);
			return;
		}
	}
}

/* discard the rest of an over-long line */
static void
skip_line(FILE *conf)
{
	int chr;

	do {
		chr = getc(conf);
	} while (chr != EOF && chr != '\n');
}

/* reads one line into buf (MAX_LINE bytes), strips trailing whitespace */
static int
read_line(FILE *conf, char *buf)
{
	size_t len;

	if (!fgets(buf, MAX_LINE, conf))
		return LINE_EOF;
	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		len--;
	else if (!feof(conf)) {
		skip_line(conf);
		return LINE_TOO_LONG;
	}
	while (len > 0 && isspace((unsigned char)buf[len - 1]))
		len--; /* also eats the CR of CRLF files */
	buf[len] = '\0';
	return LINE_OK;
}

/* opens the config if it is a regular file we trust, else returns NULL */
static FILE *
open_config(State *state)
{
	struct stat info;
	FILE *conf;
	int desc;

	desc = open(CONFIG_FILE, O_RDONLY | O_CLOEXEC);
	if (desc < 0) {
		if (errno != ENOENT)
			syslog(LOG_ERR, "cannot open %s: %s", CONFIG_FILE,
			       strerror(errno));
		return NULL;
	}
	if (fstat(desc, &info) < 0) {
		close(desc);
		return NULL;
	}
	state->report = (info.st_mtime != state->seen_mtime);
	state->seen_mtime = info.st_mtime;

	if (!S_ISREG(info.st_mode) || !trusted(&info)) {
		if (state->report)
			syslog(LOG_ERR,
			       "%s must be a regular file owned by "
			       "us and not group/world writable",
			       CONFIG_FILE);
		close(desc);
		return NULL;
	}
	conf = fdopen(desc, "r");
	if (!conf)
		close(desc);
	return conf;
}

static void
run_jobs(State *state, const Window *window)
{
	char buf[MAX_LINE];
	const char *cursor;
	FILE *conf = open_config(state);
	int status;

	if (!conf)
		return;
	while ((status = read_line(conf, buf)) != LINE_EOF) {
		if (status == LINE_TOO_LONG) {
			if (state->report)
				syslog(LOG_ERR, "config: over-long line "
						"ignored");
			continue;
		}
		for (cursor = buf; isspace((unsigned char)*cursor); cursor++)
			;
		if (*cursor != '\0' && *cursor != '#')
			process_job(state, window, cursor);
	}
	fclose(conf);
}

/* processes everything between the last tick and now, returns the new tick */
static time_t
run_tick(State *state, time_t last)
{
	time_t current = minute_floor(time(NULL));
	Window window;

	if (last != 0 && current < last) {
		/* clock went back: run nothing until we are past `last` */
		write_tick(state, current);
		return current;
	}
	if (last == 0 || current - last > MAX_CATCHUP)
		window.first = current; /* no data, or gap too large */
	else
		window.first = last + SECONDS_PER_MINUTE;
	window.last = current;
	if (window.first > window.last)
		return last; /* woke up early, minute already done */

	run_jobs(state, &window);
	write_tick(state, current);
	return current;
}

int
main(int argc, char *argv[])
{
	State state = { NO_FD, NO_FD, 0, 0 };
	time_t last;
	int foreground;

	if (argc == 2 && strcmp(argv[1], "-v") == 0) {
		puts("mcron " VERSION);
		return EXIT_SUCCESS;
	}
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "-f") != 0)) {
		fputs("usage: mcron [-f | -v]\n", stderr);
		return EXIT_FAILURE;
	}
	foreground = (argc == 2);

	openlog("mcron", foreground ? LOG_PID | LOG_PERROR : LOG_PID, LOG_CRON);
	open_statedir(&state);
	lock_instance(&state);
	if (!foreground)
		daemonize();
	write_pid(&state);
	signal(SIGCHLD, SIG_IGN); /* no zombies, no waitpid() needed */

	last = read_tick(&state);
	for (;;) {
		last = run_tick(&state, last);
		sleep((unsigned int)(SECONDS_PER_MINUTE -
				     (time(NULL) % SECONDS_PER_MINUTE)));
	}
}
