/* Copyright (c) 2014-2015 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "connection.h"
#include "istream.h"
#include "istream-unix.h"
#include "ostream.h"
#include "base64.h"
#include "strescape.h"
#include "master-service.h"
#include "mail-storage-service.h"
#include "imap-client.h"
#include "imap-state.h"
#include "imap-master-client.h"

struct imap_master_client {
	struct connection conn;
	bool imap_client_created;
};

struct imap_master_input {
	/* input we've already read from the IMAP client. */
	buffer_t *client_input;
	/* output that imap-hibernate was supposed to send to IMAP client,
	   but couldn't send it yet. */
	buffer_t *client_output;
	/* IMAP connection state */
	buffer_t *state;

	struct ip_addr peer_ip;
	in_port_t peer_port;

	bool state_import_bad_idle_done;
	bool state_import_idle_continue;
};

static struct connection_list *master_clients = NULL;

static void imap_master_client_destroy(struct connection *conn)
{
	struct imap_master_client *client = (struct imap_master_client *)conn;

	if (!client->imap_client_created)
		master_service_client_connection_destroyed(master_service);
	connection_deinit(conn);
	i_free(conn);
}

static int
imap_master_client_parse_input(const char *const *args, pool_t pool,
			       struct mail_storage_service_input *input_r,
			       struct imap_master_input *master_input_r,
			       const char **error_r)
{
	const char *key, *value;

	memset(input_r, 0, sizeof(*input_r));
	memset(master_input_r, 0, sizeof(*master_input_r));
	master_input_r->client_input = buffer_create_dynamic(pool, 64);
	master_input_r->client_output = buffer_create_dynamic(pool, 16);
	master_input_r->state = buffer_create_dynamic(pool, 512);

	input_r->module = input_r->service = "imap";
	/* we never want to do userdb lookup again when restoring the client.
	   we have the userdb_fields cached already. */
	input_r->flags_override_remove = MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;

	if (args[0] == NULL) {
		*error_r = "Missing username in input";
		return -1;
	}
	input_r->username = args[0];

	for (args++; *args != NULL; args++) {
		value = strchr(*args, '=');
		if (value != NULL)
			key = t_strdup_until(*args, value++);
		else {
			key = *args;
			value = "";
		}
		if (strcmp(key, "lip") == 0) {
			if (net_addr2ip(value, &input_r->local_ip) < 0) {
				*error_r = t_strdup_printf(
					"Invalid lip value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "rip") == 0) {
			if (net_addr2ip(value, &input_r->remote_ip) < 0) {
				*error_r = t_strdup_printf(
					"Invalid rip value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "peer_ip") == 0) {
			if (net_addr2ip(value, &master_input_r->peer_ip) < 0) {
				*error_r = t_strdup_printf(
					"Invalid peer_ip value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "peer_port") == 0) {
			if (net_str2port(value, &master_input_r->peer_port) < 0) {
				*error_r = t_strdup_printf(
					"Invalid peer_port value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "session") == 0) {
			input_r->session_id = value;
		} else if (strcmp(key, "userdb_fields") == 0) {
			input_r->userdb_fields =
				t_strsplit_tabescaped(value);
		} else if (strcmp(key, "client_input") == 0) {
			if (base64_decode(value, strlen(value), NULL,
					  master_input_r->client_input) < 0) {
				*error_r = t_strdup_printf(
					"Invalid client_input base64 value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "client_output") == 0) {
			if (base64_decode(value, strlen(value), NULL,
					  master_input_r->client_output) < 0) {
				*error_r = t_strdup_printf(
					"Invalid client_output base64 value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "state") == 0) {
			if (base64_decode(value, strlen(value), NULL,
					  master_input_r->state) < 0) {
				*error_r = t_strdup_printf(
					"Invalid state base64 value: %s", value);
				return -1;
			}
		} else if (strcmp(key, "bad-done") == 0) {
			master_input_r->state_import_bad_idle_done = TRUE;
		} else if (strcmp(key, "idle-continue") == 0) {
			master_input_r->state_import_idle_continue = TRUE;
		}
	}
	return 0;
}

static int imap_master_client_verify(const struct imap_master_input *master_input,
				     int fd_client, const char **error_r)
{
	struct ip_addr peer_ip;
	in_port_t peer_port;

	if (master_input->peer_port == 0)
		return 0;

	/* make sure we have the right fd */
	if (net_getpeername(fd_client, &peer_ip, &peer_port) < 0) {
		*error_r = t_strdup_printf("net_getpeername() failed: %m");
		return -1;
	}
	if (!net_ip_compare(&peer_ip, &master_input->peer_ip) ||
	    peer_port != master_input->peer_port) {
		*error_r = t_strdup_printf(
			"BUG: Expected peer_ip=%s peer_port=%u doesn't match "
			"client fd's actual ip=%s port=%u",
			net_ip2addr(&master_input->peer_ip),
			master_input->peer_port,
			net_ip2addr(&peer_ip), peer_port);
		return -1;
	}
	return 0;
}

static int
imap_master_client_input_args(struct connection *conn, const char *const *args,
			      int fd_client, pool_t pool)
{
	struct imap_master_client *client = (struct imap_master_client *)conn;
	struct client *imap_client;
	struct mail_storage_service_input input;
	struct imap_master_input master_input;
	const char *error;
	int ret;

	if (imap_master_client_parse_input(args, pool, &input, &master_input,
					   &error) < 0) {
		i_error("imap-master: Failed to parse client input: %s", error);
		o_stream_send_str(conn->output, t_strdup_printf(
			"-Failed to parse client input: %s\n", error));
		i_close_fd(&fd_client);
		return -1;
	}
	if (imap_master_client_verify(&master_input, fd_client, &error) < 0) {
		i_error("imap-master: Failed to verify client input: %s", error);
		o_stream_send_str(conn->output, t_strdup_printf(
			"-Failed to verify client input: %s\n", error));
		i_close_fd(&fd_client);
		return -1;
	}

	/* NOTE: before client_create_from_input() on failures we need to close
	   fd_client, but afterward it gets closed by client_destroy() */
	ret = client_create_from_input(&input, fd_client, fd_client,
				       &imap_client, &error);
	if (ret < 0) {
		i_error("imap-master(%s): Failed to create client: %s",
			input.username, error);
		o_stream_send_str(conn->output, t_strdup_printf(
			"-Failed to create client: %s\n", error));
		master_service_client_connection_destroyed(master_service);
		i_close_fd(&fd_client);
		return -1;
	}
	/* log prefix is set at this point, so we don't need to add the
	   username anymore to the log messages */
	client->imap_client_created = TRUE;

	o_stream_nsend(imap_client->output,
		       master_input.client_output->data,
		       master_input.client_output->used);
	if (master_input.client_input->used > 0 &&
	    !i_stream_add_data(imap_client->input,
			       master_input.client_input->data,
			       master_input.client_input->used)) {
		i_error("imap-master: Couldn't add %"PRIuSIZE_T
			" bytes to client's input stream",
			master_input.client_input->used);
		o_stream_send_str(conn->output,
				  "-Couldn't add client input\n");
		client_destroy(imap_client, "Client initialization failed");
		return -1;
	}
	imap_client->state_import_bad_idle_done =
		master_input.state_import_bad_idle_done;
	imap_client->state_import_idle_continue =
		master_input.state_import_idle_continue;
	ret = imap_state_import_internal(imap_client, master_input.state->data,
					 master_input.state->used, &error);
	if (ret <= 0) {
		i_error("imap-master: Failed to import client state: %s", error);
		client_destroy(imap_client, "Client state initialization failed");
		return -1;
	}

	/* make sure all pending input gets handled */
	i_assert(imap_client->to_delayed_input == NULL);
	if (master_input.client_input->used > 0) {
		imap_client->to_delayed_input =
			timeout_add(0, client_input, imap_client);
	}

	o_stream_send_str(conn->output, "+\n");
	imap_refresh_proctitle();
	/* we'll always disconnect the client afterwards */
	return -1;
}

static int
imap_master_client_input_line(struct connection *conn, const char *line)
{
	char *const *args;
	pool_t pool;
	int fd_client, ret;

	if (!conn->version_received) {
		if (connection_verify_version(conn, t_strsplit_tabescaped(line)) < 0)
			return -1;
		conn->version_received = TRUE;
		return 1;
	}

	fd_client = i_stream_unix_get_read_fd(conn->input);
	if (fd_client == -1) {
		i_error("imap-master: IMAP client fd not received");
		return -1;
	}

	if (imap_debug)
		i_debug("imap-master: Client input: %s", line);

	pool = pool_alloconly_create("imap master client cmd", 1024);
	args = p_strsplit_tabescaped(pool, line);
	ret = imap_master_client_input_args(conn, (void *)args, fd_client, pool);
	pool_unref(&pool);
	return ret;
}

void imap_master_client_create(int fd)
{
	struct imap_master_client *client;

	client = i_new(struct imap_master_client, 1);
	connection_init_server(master_clients, &client->conn,
			       "imap-master", fd, fd);

	i_assert(client->conn.input == NULL);
	client->conn.input = i_stream_create_unix(fd, (size_t)-1);
	/* read the first file descriptor that we can */
	i_stream_unix_set_read_fd(client->conn.input);
}

static struct connection_settings client_set = {
	.service_name_in = "imap-master",
	.service_name_out = "imap-master",
	.major_version = 1,
	.minor_version = 0,

	.input_max_size = 0, /* don't auto-create istream */
	.output_max_size = (size_t)-1,
	.client = FALSE
};

static const struct connection_vfuncs client_vfuncs = {
	.destroy = imap_master_client_destroy,
	.input_line = imap_master_client_input_line
};

void imap_master_clients_init(void)
{
	master_clients = connection_list_init(&client_set, &client_vfuncs);
}

void imap_master_clients_deinit(void)
{
	connection_list_deinit(&master_clients);
}