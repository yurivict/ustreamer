#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <assert.h>

#include "../libs/config.h"
#include "../libs/tools.h"
#include "../libs/logging.h"
#include "../libs/frame.h"
#include "../libs/memsink.h"
#include "../libs/base64.h"


enum _OPT_VALUES {
	_O_SINK = 's',
	_O_SINK_TIMEOUT = 't',
	_O_OUTPUT = 'o',
	_O_OUTPUT_JSON = 'j',

	_O_HELP = 'h',
	_O_VERSION = 'v',

	_O_LOG_LEVEL = 10000,
	_O_PERF,
	_O_VERBOSE,
	_O_DEBUG,
	_O_FORCE_LOG_COLORS,
	_O_NO_LOG_COLORS,
};

static const struct option _LONG_OPTS[] = {
	{"sink",				required_argument,	NULL,	_O_SINK},
	{"sink-timeout",		required_argument,	NULL,	_O_SINK_TIMEOUT},
	{"output",				required_argument,	NULL,	_O_OUTPUT},
	{"output-json",			no_argument,		NULL,	_O_OUTPUT_JSON},

	{"log-level",			required_argument,	NULL,	_O_LOG_LEVEL},
	{"perf",				no_argument,		NULL,	_O_PERF},
	{"verbose",				no_argument,		NULL,	_O_VERBOSE},
	{"debug",				no_argument,		NULL,	_O_DEBUG},
	{"force-log-colors",	no_argument,		NULL,	_O_FORCE_LOG_COLORS},
	{"no-log-colors",		no_argument,		NULL,	_O_NO_LOG_COLORS},

	{"help",				no_argument,		NULL,	_O_HELP},
	{"version",				no_argument,		NULL,	_O_VERSION},

	{NULL, 0, NULL, 0},
};


volatile bool stop = false;


static void _signal_handler(int signum);
static void _install_signal_handlers(void);

static int _dump_sink(const char *sink_name, unsigned sink_timeout, const char *output_path, bool output_json);

static void _help(FILE *fp);


int main(int argc, char *argv[]) {
	LOGGING_INIT;
	A_THREAD_RENAME("main");

	char *sink_name = NULL;
	unsigned sink_timeout = 1;
	char *output_path = NULL;
	bool output_json = false;

#	define OPT_SET(_dest, _value) { \
			_dest = _value; \
			break; \
		}

#	define OPT_NUMBER(_name, _dest, _min, _max, _base) { \
			errno = 0; char *_end = NULL; long long _tmp = strtoll(optarg, &_end, _base); \
			if (errno || *_end || _tmp < _min || _tmp > _max) { \
				printf("Invalid value for '%s=%s': min=%lld, max=%lld\n", _name, optarg, (long long)_min, (long long)_max); \
				return 1; \
			} \
			_dest = _tmp; \
			break; \
		}

	for (int ch; (ch = getopt_long(argc, argv, "s:t:o:jhv", _LONG_OPTS, NULL)) >= 0;) {
		switch (ch) {
			case _O_SINK:			OPT_SET(sink_name, optarg);
			case _O_SINK_TIMEOUT:	OPT_NUMBER("--sink-timeout", sink_timeout, 1, 60, 0);
			case _O_OUTPUT:			OPT_SET(output_path, optarg);
			case _O_OUTPUT_JSON:	OPT_SET(output_json, true);

			case _O_LOG_LEVEL:			OPT_NUMBER("--log-level", log_level, LOG_LEVEL_INFO, LOG_LEVEL_DEBUG, 0);
			case _O_PERF:				OPT_SET(log_level, LOG_LEVEL_PERF);
			case _O_VERBOSE:			OPT_SET(log_level, LOG_LEVEL_VERBOSE);
			case _O_DEBUG:				OPT_SET(log_level, LOG_LEVEL_DEBUG);
			case _O_FORCE_LOG_COLORS:	OPT_SET(log_colored, true);
			case _O_NO_LOG_COLORS:		OPT_SET(log_colored, false);

			case _O_HELP:		_help(stdout); return 0;
			case _O_VERSION:	puts(VERSION); return 0;

			case 0:		break;
			default:	_help(stderr); return 1;
		}
	}

#	undef OPT_NUMBER
#	undef OPT_SET

	if (sink_name == NULL || sink_name[0] == '\0') {
		puts("Missing option --sink. See --help for details.");
		return 1;
	}

	_install_signal_handlers();
	return abs(_dump_sink(sink_name, sink_timeout, output_path, output_json));
}


static void _signal_handler(int signum) {
	switch (signum) {
		case SIGTERM:	LOG_INFO_NOLOCK("===== Stopping by SIGTERM ====="); break;
		case SIGINT:	LOG_INFO_NOLOCK("===== Stopping by SIGINT ====="); break;
		case SIGPIPE:	LOG_INFO_NOLOCK("===== Stopping by SIGPIPE ====="); break;
		default:		LOG_INFO_NOLOCK("===== Stopping by %d =====", signum); break;
	}
	stop = true;
}

static void _install_signal_handlers(void) {
	struct sigaction sig_act;
	MEMSET_ZERO(sig_act);

	assert(!sigemptyset(&sig_act.sa_mask));
	sig_act.sa_handler = _signal_handler;
	assert(!sigaddset(&sig_act.sa_mask, SIGINT));
	assert(!sigaddset(&sig_act.sa_mask, SIGTERM));
	assert(!sigaddset(&sig_act.sa_mask, SIGPIPE));

	LOG_DEBUG("Installing SIGINT handler ...");
	assert(!sigaction(SIGINT, &sig_act, NULL));

	LOG_DEBUG("Installing SIGTERM handler ...");
	assert(!sigaction(SIGTERM, &sig_act, NULL));

	LOG_DEBUG("Installing SIGTERM handler ...");
	assert(!sigaction(SIGPIPE, &sig_act, NULL));
}

static int _dump_sink(const char *sink_name, unsigned sink_timeout, const char *output_path, bool output_json) {
	frame_s *frame = frame_init("input");
	memsink_s *sink = NULL;
	FILE *output_fp = NULL;
	char *base64_data = NULL;
	size_t base64_allocated = 0;

	if (output_path && output_path[0] != '\0') {
		if (!strcmp(output_path, "-")) {
			LOG_INFO("Using output: <stdout>");
			output_fp = stdout;
		} else {
			LOG_INFO("Using output: %s", output_path);
			if ((output_fp = fopen(output_path, "wb")) == NULL) {
				LOG_PERROR("Can't open output file");
				goto error;
			}
		}
	}

	if ((sink = memsink_init("input", sink_name, false, 0, false, sink_timeout)) == NULL) {
		goto error;
	}

	unsigned fps = 0;
	unsigned fps_accum = 0;
	long long fps_second = 0;

	while (!stop) {
		int error = memsink_client_get(sink, frame);
		if (error == 0) {
			const long double now = get_now_monotonic();
			const long long now_second = floor_ms(now);

			char fourcc_str[8];
			LOG_VERBOSE("Frame: size=%zu, resolution=%ux%u, fourcc=%s, stride=%u, online=%d",
				frame->used, frame->width, frame->height,
				fourcc_to_string(frame->format, fourcc_str, 8),
				frame->stride, frame->online);

			LOG_DEBUG("       grab_ts=%.3Lf, encode_begin_ts=%.3Lf, encode_end_ts=%.3Lf, latency=%.3Lf",
				frame->grab_ts, frame->encode_begin_ts, frame->encode_end_ts, now - frame->grab_ts);

			if (now_second != fps_second) {
				fps = fps_accum;
				fps_accum = 0;
				fps_second = now_second;
				LOG_PERF_FPS("A new second has come; captured_fps=%u", fps);
			}
			fps_accum += 1;

			if (output_fp) {
				if (output_json) {
					base64_encode(frame->data, frame->used, &base64_data, &base64_allocated);
					fprintf(output_fp,
						"{\"size\": %zu, \"width\": %u, \"height\": %u,"
						" \"format\": %u, \"stride\": %u, \"online\": %u,"
						" \"grab_ts\": %.3Lf, \"encode_begin_ts\": %.3Lf, \"encode_end_ts\": %.3Lf,"
						" \"data\": \"%s\"}\n",
						frame->used, frame->width, frame->height,
						frame->format, frame->stride, frame->online,
						frame->grab_ts, frame->encode_begin_ts, frame->encode_end_ts,
						base64_data);
				} else {
					fwrite(frame->data, 1, frame->used, output_fp);
				}
				fflush(output_fp);
			}
		} else if (error != -2) {
			goto error;
		}
	}

	int retval = 0;
	goto ok;

	error:
		retval = -1;

	ok:
		if (base64_data) {
			free(base64_data);
		}
		if (output_fp && output_fp != stdout) {
			if (fclose(output_fp) < 0) {
				LOG_PERROR("Can't close output file");
			}
		}
		if (sink) {
			memsink_destroy(sink);
		}
		frame_destroy(frame);

		LOG_INFO("Bye-bye");
		return retval;
}

static void _help(FILE *fp) {
#	define SAY(_msg, ...) fprintf(fp, _msg "\n", ##__VA_ARGS__)
	SAY("\nuStreamer-dump - Dump uStreamer's memory sink to file");
	SAY("═════════════════════════════════════════════════════");
	SAY("Version: %s; license: GPLv3", VERSION);
	SAY("Copyright (C) 2018 Maxim Devaev <mdevaev@gmail.com>\n");
	SAY("Example:");
	SAY("════════");
	SAY("    ustreamer-dump --sink test --output - \\");
	SAY("        | ffmpeg -use_wallclock_as_timestamps 1 -i pipe: -c:v libx264 test.mp4\n");
	SAY("Sink options:");
	SAY("═════════════");
	SAY("    -s|--sink <name>  ──────── Memory sink ID. No default.\n");
	SAY("    -t|--sink-timeout <sec>  ─ Timeout for the upcoming frame. Default: 1.\n");
	SAY("    -o|--output  ───────────── Filename to dump. Use '-' for stdout. Default: just consume the sink.\n");
	SAY("    -j|--output-json  ──────── Format output as JSON. Required option --output. Default: disabled.\n");
	SAY("Logging options:");
	SAY("════════════════");
	SAY("    --log-level <N>  ──── Verbosity level of messages from 0 (info) to 3 (debug).");
	SAY("                          Enabling debugging messages can slow down the program.");
	SAY("                          Available levels: 0 (info), 1 (performance), 2 (verbose), 3 (debug).");
	SAY("                          Default: %d.\n", log_level);
	SAY("    --perf  ───────────── Enable performance messages (same as --log-level=1). Default: disabled.\n");
	SAY("    --verbose  ────────── Enable verbose messages and lower (same as --log-level=2). Default: disabled.\n");
	SAY("    --debug  ──────────── Enable debug messages and lower (same as --log-level=3). Default: disabled.\n");
	SAY("    --force-log-colors  ─ Force color logging. Default: colored if stderr is a TTY.\n");
	SAY("    --no-log-colors  ──── Disable color logging. Default: ditto.\n");
	SAY("Help options:");
	SAY("═════════════");
	SAY("    -h|--help  ─────── Print this text and exit.\n");
	SAY("    -v|--version  ──── Print version and exit.\n");
#	undef SAY
}
