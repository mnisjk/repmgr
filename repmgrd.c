/*
 * repmgrd.c - Replication manager daemon
 *
 * Copyright (c) 2ndQuadrant, 2010-2018
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>


#include "repmgr.h"
#include "repmgrd.h"
#include "repmgrd-physical.h"
#include "repmgrd-bdr.h"
#include "configfile.h"
#include "voting.h"

#define OPT_HELP	1


static char *config_file = NULL;
static bool verbose = false;
static char *pid_file = NULL;
static bool daemonize = false;

t_configuration_options config_file_options = T_CONFIGURATION_OPTIONS_INITIALIZER;

t_node_info local_node_info = T_NODE_INFO_INITIALIZER;
PGconn	   *local_conn = NULL;



/* Collate command line errors here for friendlier reporting */
static ItemList cli_errors = {NULL, NULL};

bool		startup_event_logged = false;

MonitoringState monitoring_state = MS_NORMAL;
instr_time	degraded_monitoring_start;

/*
 * Record receipt of SIGHUP; will cause configuration file to be reread
 * at the appropriate point in the main loop.
 */
volatile sig_atomic_t got_SIGHUP = false;

static void show_help(void);
static void show_usage(void);
static void daemonize_process(void);
static void check_and_create_pid_file(const char *pid_file);

static void start_monitoring(void);


#ifndef WIN32
static void setup_event_handlers(void);
static void handle_sighup(SIGNAL_ARGS);
#endif

int			calculate_elapsed(instr_time start_time);
void		update_registration(PGconn *conn);
void		terminate(int retval);

int
main(int argc, char **argv)
{
	int			optindex;
	int			c;
	char		cli_log_level[MAXLEN] = "";
	bool		cli_monitoring_history = false;

	RecordStatus record_status;
	ExtensionStatus extension_status = REPMGR_UNKNOWN;

	FILE	   *fd;

	static struct option long_options[] =
	{
/* general options */
		{"help", no_argument, NULL, OPT_HELP},
		{"version", no_argument, NULL, 'V'},

/* configuration options */
		{"config-file", required_argument, NULL, 'f'},

/* daemon options */
		{"daemonize", no_argument, NULL, 'd'},
		{"pid-file", required_argument, NULL, 'p'},

/* logging options */
		{"log-level", required_argument, NULL, 'L'},
		{"verbose", no_argument, NULL, 'v'},

/* legacy options */
		{"monitoring-history", no_argument, NULL, 'm'},
		{NULL, 0, NULL, 0}
	};

	set_progname(argv[0]);

	srand(time(NULL));

	/* Disallow running as root */
	if (geteuid() == 0)
	{
		fprintf(stderr,
				_("%s: cannot be run as root\n"
				  "Please log in (using, e.g., \"su\") as the "
				  "(unprivileged) user that owns "
				  "the data directory.\n"
				  ),
				progname());
		exit(1);
	}

	while ((c = getopt_long(argc, argv, "?Vf:L:vdp:m", long_options, &optindex)) != -1)
	{
		switch (c)
		{

				/* general options */

			case '?':
				/* Actual help option given */
				if (strcmp(argv[optind - 1], "-?") == 0)
				{
					show_help();
					exit(SUCCESS);
				}
				/* unknown option reported by getopt */
				goto unknown_option;
				break;

			case OPT_HELP:
				show_help();
				exit(SUCCESS);

			case 'V':

				/*
				 * in contrast to repmgr3 and earlier, we only display the
				 * repmgr version as it's not specific to a particular
				 * PostgreSQL version
				 */
				printf("%s %s\n", progname(), REPMGR_VERSION);
				exit(SUCCESS);

				/* configuration options */

			case 'f':
				config_file = optarg;
				break;

				/* daemon options */

			case 'd':
				daemonize = true;
				break;

			case 'p':
				pid_file = optarg;
				break;

				/* logging options */

				/* -L/--log-level */
			case 'L':
				{
					int			detected_cli_log_level = detect_log_level(optarg);

					if (detected_cli_log_level != -1)
					{
						strncpy(cli_log_level, optarg, MAXLEN);
					}
					else
					{
						PQExpBufferData invalid_log_level;

						initPQExpBuffer(&invalid_log_level);
						appendPQExpBuffer(&invalid_log_level,
										  _("invalid log level \"%s\" provided"),
										  optarg);
						item_list_append(&cli_errors, invalid_log_level.data);
						termPQExpBuffer(&invalid_log_level);
					}
					break;
				}
			case 'v':
				verbose = true;
				break;

				/* legacy options */

			case 'm':
				cli_monitoring_history = true;
				break;

			default:
		unknown_option:
				show_usage();
				exit(ERR_BAD_CONFIG);
		}
	}

	/* Exit here already if errors in command line options found */
	if (cli_errors.head != NULL)
	{
		exit_with_cli_errors(&cli_errors);
	}

	startup_event_logged = false;

	/*
	 * Tell the logger we're a daemon - this will ensure any output logged
	 * before the logger is initialized will be formatted correctly
	 */
	logger_output_mode = OM_DAEMON;

	/*
	 * Parse the configuration file, if provided (if no configuration file was
	 * provided, an attempt will be made to find one in one of the default
	 * locations). If no conifguration file is available, or it can't be parsed
	 * parse_config() will abort anyway, with an appropriate message.
	 */
	load_config(config_file, verbose, false, &config_file_options, argv[0]);


	/* Some configuration file items can be overriden by command line options */

	/*
	 * Command-line parameter -L/--log-level overrides any setting in config
	 * file
	 */
	if (*cli_log_level != '\0')
	{
		strncpy(config_file_options.log_level, cli_log_level, MAXLEN);
	}

	log_notice(_("repmgrd (repmgr %s) starting up"), REPMGR_VERSION);

	/*
	 * -m/--monitoring-history, if provided, will override repmgr.conf's
	 * monitoring_history; this is for backwards compatibility as it's
	 * possible this may be baked into various startup scripts.
	 */

	if (cli_monitoring_history == true)
	{
		config_file_options.monitoring_history = true;
	}

	fd = freopen("/dev/null", "r", stdin);
	if (fd == NULL)
	{
		fprintf(stderr, "error reopening stdin to \"/dev/null\":\n  %s\n",
				strerror(errno));
	}

	fd = freopen("/dev/null", "w", stdout);
	if (fd == NULL)
	{
		fprintf(stderr, "error reopening stdout to \"/dev/null\":\n  %s\n",
				strerror(errno));
	}

	logger_init(&config_file_options, progname());

	if (verbose)
		logger_set_verbose();

	if (log_type == REPMGR_SYSLOG)
	{
		fd = freopen("/dev/null", "w", stderr);

		if (fd == NULL)
		{
			fprintf(stderr, "error reopening stderr to \"/dev/null\":\n  %s\n",
					strerror(errno));
		}
	}

	log_info(_("connecting to database \"%s\""),
			 config_file_options.conninfo);

	/* abort if local node not available at startup */
	local_conn = establish_db_connection(config_file_options.conninfo, true);

	/*
	 * store the server version number - we'll need this to generate
	 * version-dependent queries etc.
	 */
	server_version_num = get_server_version(local_conn, NULL);

	/*
	 * sanity checks
	 *
	 * Note: previous repmgr versions checked the PostgreSQL version at this
	 * point, but we'll skip that and assume the presence of a node record
	 * means we're dealing with a supported installation.
	 *
	 * The absence of a node record will also indicate that either the node or
	 * repmgr has not been properly configured.
	 */

	/* Check "repmgr" the extension is installed */
	extension_status = get_repmgr_extension_status(local_conn);

	if (extension_status != REPMGR_INSTALLED)
	{
		/* this is unlikely to happen */
		if (extension_status == REPMGR_UNKNOWN)
		{
			log_error(_("unable to determine status of \"repmgr\" extension"));
			log_detail("%s", PQerrorMessage(local_conn));
			close_connection(&local_conn);
			exit(ERR_DB_QUERY);
		}

		log_error(_("repmgr extension not found on this node"));

		if (extension_status == REPMGR_AVAILABLE)
		{
			log_detail(_("repmgr extension is available but not installed in database \"%s\""),
					   PQdb(local_conn));
		}
		else if (extension_status == REPMGR_UNAVAILABLE)
		{
			log_detail(_("repmgr extension is not available on this node"));
		}

		log_hint(_("check that this node is part of a repmgr cluster"));
		close_connection(&local_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Retrieve record for this node from the local database */
	record_status = get_node_record(local_conn, config_file_options.node_id, &local_node_info);

	/*
	 * Terminate if we can't find the local node record. This is a
	 * "fix-the-config" situation, not a lot else we can do.
	 */

	if (record_status != RECORD_FOUND)
	{
		log_error(_("no metadata record found for this node - terminating"));

		switch (config_file_options.replication_type)
		{
			case REPLICATION_TYPE_PHYSICAL:
				log_hint(_("check that 'repmgr (primary|standby) register' was executed for this node"));
				break;
			case REPLICATION_TYPE_BDR:
				log_hint(_("check that 'repmgr bdr register' was executed for this node"));
				break;
		}

		close_connection(&local_conn);
		terminate(ERR_BAD_CONFIG);
	}

	repmgrd_set_local_node_id(local_conn, config_file_options.node_id);

	{
		/*
		 * sanity-check that the shared library is loaded and shared memory
		 * can be written by attempting to retrieve the previously stored node_id
		 */
		int stored_local_node_id = UNKNOWN_NODE_ID;

		stored_local_node_id = repmgrd_get_local_node_id(local_conn);

		if (stored_local_node_id == UNKNOWN_NODE_ID)
		{
			log_error(_("unable to write to shared memory"));
			log_hint(_("ensure \"shared_preload_libraries\" includes \"repmgr\""));
			close_connection(&local_conn);
			terminate(ERR_BAD_CONFIG);
		}
	}

	if (config_file_options.replication_type == REPLICATION_TYPE_BDR)
	{
		log_debug("node id is %i", local_node_info.node_id);
		do_bdr_node_check();
	}
	else
	{
		log_debug("node id is %i, upstream node id is %i",
				  local_node_info.node_id,
				  local_node_info.upstream_node_id);
		do_physical_node_check();
	}



	if (daemonize == true)
	{
		daemonize_process();
	}

	if (pid_file != NULL)
	{
		check_and_create_pid_file(pid_file);
	}

#ifndef WIN32
	setup_event_handlers();
#endif

	start_monitoring();

	logger_shutdown();

	return SUCCESS;
}



static void
start_monitoring(void)
{
	log_notice(_("starting monitoring of node \"%s\" (ID: %i)"),
			   local_node_info.node_name,
			   local_node_info.node_id);

	while (true)
	{
		switch (local_node_info.type)
		{
			case PRIMARY:
				monitor_streaming_primary();
				break;
			case STANDBY:
				monitor_streaming_standby();
				break;
			case WITNESS:
				monitor_streaming_witness();
				break;
			case BDR:
				monitor_bdr();
				return;
			case UNKNOWN:
				/* should never happen */
				break;
		}
	}
}


void
update_registration(PGconn *conn)
{
	bool		success = update_node_record_conn_priority(local_conn,
														   &config_file_options);

	/* check values have actually changed */

	if (success == false)
	{
		PQExpBufferData errmsg;

		initPQExpBuffer(&errmsg);

		appendPQExpBuffer(&errmsg,
						  _("unable to update local node record:\n  %s"),
						  PQerrorMessage(conn));

		create_event_record(conn,
							&config_file_options,
							config_file_options.node_id,
							"repmgrd_config_reload",
							false,
							errmsg.data);
		termPQExpBuffer(&errmsg);
	}

	return;
}


static void
daemonize_process(void)
{
	char	   *ptr,
				path[MAXPGPATH];
	pid_t		pid = fork();
	int			ret;

	switch (pid)
	{
		case -1:
			log_error(_("error in fork():\n  %s"), strerror(errno));
			exit(ERR_SYS_FAILURE);
			break;

		case 0:
			/* create independent session ID */
			pid = setsid();
			if (pid == (pid_t) -1)
			{
				log_error(_("error in setsid():\n  %s"), strerror(errno));
				exit(ERR_SYS_FAILURE);
			}

			/* ensure that we are no longer able to open a terminal */
			pid = fork();

			/* error case */
			if (pid == -1)
			{
				log_error(_("error in fork():\n  %s"), strerror(errno));
				exit(ERR_SYS_FAILURE);
			}

			/* parent process */
			if (pid != 0)
			{
				exit(0);
			}

			/* child process */

			memset(path, 0, MAXPGPATH);

			for (ptr = config_file_path + strlen(config_file_path); ptr > config_file_path; --ptr)
			{
				if (*ptr == '/')
				{
					strncpy(path, config_file_path, ptr - config_file_path);
				}
			}

			if (*path == '\0')
			{
				*path = '/';
			}

			log_debug("dir now %s", path);
			ret = chdir(path);
			if (ret != 0)
			{
				log_error(_("error changing directory to \"%s\":\n  %s"), path,
						  strerror(errno));
			}

			break;

		default:				/* parent process */
			exit(0);
	}
}

static void
check_and_create_pid_file(const char *pid_file)
{
	struct stat st;
	FILE	   *fd;
	char		buff[MAXLEN];
	pid_t		pid;
	size_t		nread;

	if (stat(pid_file, &st) != -1)
	{
		memset(buff, 0, MAXLEN);

		fd = fopen(pid_file, "r");

		if (fd == NULL)
		{
			log_error(_("PID file \"%s\" exists but could not opened for reading"), pid_file);
			log_hint(_("if repmgrd is no longer alive, remove the file and restart repmgrd"));
			exit(ERR_BAD_PIDFILE);
		}

		nread = fread(buff, MAXLEN - 1, 1, fd);

		if (nread == 0 && ferror(fd))
		{
			log_error(_("error reading PID file \"%s\", aborting"), pid_file);
			exit(ERR_BAD_PIDFILE);
		}

		fclose(fd);

		pid = atoi(buff);

		if (pid != 0)
		{
			if (kill(pid, 0) != -1)
			{
				log_error(_("PID file \"%s\" exists and seems to contain a valid PID"), pid_file);
				log_hint(_("if repmgrd is no longer alive, remove the file and restart repmgrd"));
				exit(ERR_BAD_PIDFILE);
			}
		}
	}

	fd = fopen(pid_file, "w");
	if (fd == NULL)
	{
		log_error(_("could not open PID file %s"), pid_file);
		exit(ERR_BAD_CONFIG);
	}

	fprintf(fd, "%d", getpid());
	fclose(fd);
}


#ifndef WIN32

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
handle_sighup(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

static void
setup_event_handlers(void)
{
	pqsignal(SIGHUP, handle_sighup);

	/*
	 * we want to be able to write a "repmgrd_shutdown" event, so delegate
	 * signal handling to the respective replication type handler, as it
	 * will know best which database connection to use
	 */
	switch (config_file_options.replication_type)
	{
		case REPLICATION_TYPE_BDR:
			pqsignal(SIGINT, handle_sigint_bdr);
			pqsignal(SIGTERM, handle_sigint_bdr);
			break;
		case REPLICATION_TYPE_PHYSICAL:
			pqsignal(SIGINT, handle_sigint_physical);
			pqsignal(SIGTERM, handle_sigint_physical);
			break;
	}
}
#endif


void
show_usage(void)
{
	fprintf(stderr, _("%s: replication management daemon for PostgreSQL\n"), progname());
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname());
}

void
show_help(void)
{
	printf(_("%s: replication management daemon for PostgreSQL\n"), progname());
	puts("");

	printf(_("Usage:\n"));
	printf(_("  %s [OPTIONS]\n"), progname());
	printf(_("\n"));
	printf(_("Options:\n"));
	puts("");

	printf(_("General options:\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("  -V, --version             output version information, then exit\n"));

	puts("");

	printf(_("General configuration options:\n"));
	printf(_("  -v, --verbose             output verbose activity information\n"));
	printf(_("  -f, --config-file=PATH    path to the configuration file\n"));

	puts("");

	printf(_("General configuration options:\n"));
	printf(_("  -d, --daemonize           detach process from foreground\n"));
	printf(_("  -p, --pid-file=PATH       write a PID file\n"));
	puts("");

	printf(_("%s monitors a cluster of servers and optionally performs failover.\n"), progname());
}


PGconn *
try_reconnect(t_node_info *node_info)
{
	PGconn	   *conn;
	t_conninfo_param_list conninfo_params = T_CONNINFO_PARAM_LIST_INITIALIZER;

	int			i;

	int			max_attempts = config_file_options.reconnect_attempts;

	initialize_conninfo_params(&conninfo_params, false);


	/* we assume by now the conninfo string is parseable */
	(void) parse_conninfo_string(node_info->conninfo, &conninfo_params, NULL, false);

	/* set some default values if not explicitly provided */
	param_set_ine(&conninfo_params, "connect_timeout", "2");
	param_set_ine(&conninfo_params, "fallback_application_name", "repmgr");

	for (i = 0; i < max_attempts; i++)
	{
		log_info(_("checking state of node %i, %i of %i attempts"),
				 node_info->node_id, i + 1, max_attempts);
		if (is_server_available_params(&conninfo_params) == true)
		{

			log_notice(_("node has recovered, reconnecting"));

			/*
			 * XXX we should also handle the case where node is pingable but
			 * connection denied due to connection exhaustion - fall back to
			 * degraded monitoring? - make that configurable
			 */

			conn = establish_db_connection_by_params(&conninfo_params, false);

			if (PQstatus(conn) == CONNECTION_OK)
			{
				free_conninfo_params(&conninfo_params);

				node_info->node_status = NODE_STATUS_UP;
				return conn;
			}

			close_connection(&conn);
			log_notice(_("unable to reconnect to node"));
		}

		if (i + 1 < max_attempts)
		{
			log_info(_("sleeping %i seconds until next reconnection attempt"),
					 config_file_options.reconnect_interval);
			sleep(config_file_options.reconnect_interval);
		}
	}

	log_warning(_("unable to reconnect to node %i after %i attempts"),
				node_info->node_id,
				max_attempts);

	node_info->node_status = NODE_STATUS_DOWN;

	free_conninfo_params(&conninfo_params);

	return NULL;
}



int
calculate_elapsed(instr_time start_time)
{
	instr_time	current_time;

	INSTR_TIME_SET_CURRENT(current_time);

	INSTR_TIME_SUBTRACT(current_time, start_time);

	return (int) INSTR_TIME_GET_DOUBLE(current_time);
}


const char *
print_monitoring_state(MonitoringState monitoring_state)
{
	switch (monitoring_state)
	{
		case MS_NORMAL:
			return "normal";

		case MS_DEGRADED:
			return "degraded";
	}

	/* should never reach here */
	return "UNKNOWN";
}


void
terminate(int retval)
{
	logger_shutdown();

	if (pid_file)
	{
		unlink(pid_file);
	}

	log_info(_("%s terminating..."), progname());

	exit(retval);
}
