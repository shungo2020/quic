#include <stdio.h>
#include <event2/event.h>
#include <memory.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <getopt.h>
#include <iostream>

using namespace std;

extern "C" xqc_usec_t xqc_now();

#define XQC_ALPN_TRANSPORT      "transport"

static int get_sys_errno() {
	return errno;
}

typedef struct client_ctx_s {
	xqc_engine_t *engine;
	struct event_base *eb;
	struct event *ev_engine;
	struct event *ev_timer;

	const xqc_cid_t *cid;
	xqc_stream_t *stream;

	int fd;
	struct event *ev_socket;
	struct sockaddr local_addr;
	socklen_t local_addr_len;

	uint32_t last_recv_bytes;
} client_ctx_t;

void xqc_log(xqc_log_level_t lvl, const void *buf, size_t count, void *engine_user_data)
{
	cout << (char*)buf << endl;
}

void xqc_client_set_event_timer(xqc_usec_t wake_after, void *user_data) {
	client_ctx_t *ctx = (client_ctx_t *) user_data;
	struct timeval tv;
	tv.tv_sec = wake_after / 1000000;
	tv.tv_usec = wake_after % 1000000;
	event_add(ctx->ev_engine, &tv);
}

void xqc_keylog_cb(const xqc_cid_t *scid, const char *line, void *user_data) {
}

ssize_t
xqc_client_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
						void *user) {
	client_ctx_t *ctx = (client_ctx_t *)user;
	ssize_t res = sendto(ctx->fd, buf, size, 0, peer_addr, peer_addrlen);
	if (res < 0) {
		printf("xqc_client_write_socket err %zd %s\n", res, strerror(get_sys_errno()));
		if (get_sys_errno() == EAGAIN) {
			res = XQC_SOCKET_EAGAIN;
		}
		if (errno == EMSGSIZE) {
			res = XQC_SOCKET_ERROR;
		}
	}
	return res;
}

void xqc_client_save_token(const unsigned char *token, unsigned token_len, void *user_data) {}

void save_session_cb(const char *data, size_t data_len, void *user_data) {}

void save_tp_cb(const char *data, size_t data_len, void *conn_user_data) {}

void xqc_client_conn_update_cid_notify(xqc_connection_t *conn, const xqc_cid_t *retire_cid, const xqc_cid_t *new_cid,
									   void *user_data) {
}

xqc_int_t
xqc_client_conn_closing_notify(xqc_connection_t *conn, const xqc_cid_t *cid, xqc_int_t err_code, void *conn_user_data) {
	printf("conn closing: %d\n", err_code);
	return XQC_OK;
}

int
xqc_client_cert_verify(const unsigned char *certs[], const size_t cert_len[], size_t certs_len, void *conn_user_data) {
	/* self-signed cert used in test cases, return >= 0 means success */
	return 0;
}

int
xqc_client_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data) {
	cout << "conn created" << endl;
	return 0;
}

int xqc_client_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data) {
	cout << "conn close" << endl;
	return 0;
}

int xqc_client_create_stream(client_ctx_t *ctx) {
	xqc_stream_settings_t setting = {
			.recv_rate_bytes_per_sec = 0,
	};
	ctx->stream = xqc_stream_create(ctx->engine, ctx->cid, &setting, ctx);
	if (ctx->stream == NULL) {
		cout << "create stream error" << endl;
		return -1;
	}
	char buf[128];
	xqc_stream_send(ctx->stream, (unsigned char *) buf, sizeof(buf), 0);
	return 0;
}

void xqc_client_conn_handshake_finished(xqc_connection_t *conn, void *user_data, void *conn_proto_data) {
	cout << "conn handshake" << endl;
	xqc_client_create_stream((client_ctx_t *) user_data);
}

int xqc_client_stream_read_notify(xqc_stream_t *stream, void *user_data) {
	char buffer[2048];
	uint8_t is_fin = 0;
	client_ctx_t *ctx = (client_ctx_t *) user_data;
	while (1) {
		ssize_t nread = xqc_stream_recv(ctx->stream, (unsigned char *) buffer, sizeof(buffer), &is_fin);
		if (nread <= 0)
			return 0;
		ctx->last_recv_bytes += (uint32_t) nread;
	}
	return 0;
}

int xqc_client_stream_close_notify(xqc_stream_t *stream, void *user_data) {
	cout << "stream close" << endl;
	return 0;
}

int xqc_client_stream_write_notify(xqc_stream_t *stream, void *user_data) {
	return 0;
}

void xqc_client_socket_event_callback(int fd, short what, void *arg) {
	client_ctx_t *ctx = (client_ctx_t *) arg;
	if (what & EV_READ) {
		char buffer[2048];
		while (1) {
			struct sockaddr peer_addr;
			socklen_t peer_addrlen = sizeof(peer_addr);
			ssize_t recv_size = recvfrom(fd,
										buffer, sizeof(buffer), 0,
										&peer_addr, &peer_addrlen);
			if (recv_size <= 0)
				break;

			if (xqc_engine_packet_process(ctx->engine, (const unsigned char *) buffer, recv_size,
									  &ctx->local_addr, ctx->local_addr_len,
									  &peer_addr, peer_addrlen,
									  xqc_now(), ctx) != XQC_OK)
									  return;
		}
		xqc_engine_finish_recv(ctx->engine);
	}
}

int xqc_client_create_engine(client_ctx_t *ctx) {
	xqc_engine_ssl_config_t ssl_cfg;
	memset((char *) &ssl_cfg, 0, sizeof(ssl_cfg));
	ssl_cfg.ciphers = (char *) XQC_TLS_CIPHERS;
	ssl_cfg.groups = (char *) XQC_TLS_GROUPS;

	xqc_engine_callback_t callback = {
			.set_event_timer = xqc_client_set_event_timer, /* call xqc_engine_main_logic when the timer expires */
			.log_callbacks = {
            	.xqc_log_write_err = xqc_log,
            	.xqc_log_write_stat = xqc_log,
        	},
			.keylog_cb = xqc_keylog_cb,
	};
	xqc_transport_callbacks_t tcbs = {
			.write_socket = xqc_client_write_socket,
			.conn_update_cid_notify = xqc_client_conn_update_cid_notify,
			.save_token = xqc_client_save_token,
			.save_session_cb = save_session_cb,
			.save_tp_cb = save_tp_cb,
			.cert_verify_cb = xqc_client_cert_verify,
			.conn_closing = xqc_client_conn_closing_notify,
	};
	xqc_config_t config;
	if (xqc_engine_get_default_config(&config, XQC_ENGINE_CLIENT)) {
		cout << "get default xquic client cfg error" << endl;
		return -1;
	}
	ctx->engine = xqc_engine_create(XQC_ENGINE_CLIENT, &config, &ssl_cfg, &callback, &tcbs, ctx);
	if (ctx->engine == NULL) {
		cout << "xqc_engine_create error" << endl;
		return -1;
	}

	xqc_app_proto_callbacks_t ap_cbs = {
			.conn_cbs = {
					.conn_create_notify = xqc_client_conn_create_notify,
					.conn_close_notify = xqc_client_conn_close_notify,
					.conn_handshake_finished = xqc_client_conn_handshake_finished,
			},
			.stream_cbs = {
					.stream_read_notify = xqc_client_stream_read_notify,
					.stream_write_notify = xqc_client_stream_write_notify,
					.stream_close_notify = xqc_client_stream_close_notify,
			},
	};
	xqc_engine_register_alpn(ctx->engine, XQC_ALPN_TRANSPORT, 9, &ap_cbs, NULL);
	return 0;
}

int xqc_client_create_conn(client_ctx_t *ctx, const char *ip, int port) {
	uint32_t cong_flags = XQC_BBR_FLAG_NONE;
	xqc_conn_settings_t setting = {
			.pacing_on  =   1,
			.ping_on    =   1,
			.cong_ctrl_callback = xqc_bbr_cb,
			.cc_params  =   {
					.customize_on = 0,
					.init_cwnd = 32,
					.cc_optimization_flags = cong_flags,
					.copa_delta_base = 1.0,
					.copa_delta_ai_unit = 1.0,
			},
			.linger = {
					.linger_on = 1,
					.linger_timeout = 500000,
			},
			.proto_version = XQC_IDRAFT_VER_NEGOTIATION,
			.init_idle_time_out = 30 * 1000,
			.idle_time_out = 30 * 1000,
			.spurious_loss_detect_on = 0,
			.keyupdate_pkt_threshold = 0,
			.max_datagram_frame_size = 1500,
			.enable_multipath = 0,
			.mp_ping_on = 0,
			.marking_reinjection = 1,
			.recv_rate_bytes_per_sec = 0,
	};
	xqc_conn_ssl_config_t conn_ssl_config;
	memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));
	conn_ssl_config.cert_verify_flag |= XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
	struct sockaddr_in addr_in;
	memset((char *) &addr_in, 0, sizeof(addr_in));
	addr_in.sin_family = AF_INET;
	addr_in.sin_port = htons(port);
	inet_pton(AF_INET, ip, &addr_in.sin_addr);
	ctx->cid = xqc_connect(ctx->engine, &setting, NULL, 0, "", 0, &conn_ssl_config, (struct sockaddr *) &addr_in,
						   sizeof(struct sockaddr_in),
						   XQC_ALPN_TRANSPORT, ctx);
	if (ctx->cid == NULL) {
		cout << "xqc connect " << ip << ":" << port << " err" << endl;
		return -1;
	}
	return 0;
}

int xqc_client_create_socket(client_ctx_t *ctx) {
	ctx->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->fd < 0) {
		cout << "create socket failed, errno" << endl;
		return -1;
	}
	if (fcntl(ctx->fd, F_SETFL, O_NONBLOCK) == -1) {
		cout << "set socket nonblock failed" << endl;
		goto err;
	}
	ctx->local_addr_len = sizeof(struct sockaddr_in6);
	getsockname(ctx->fd, (struct sockaddr *) &ctx->local_addr, &ctx->local_addr_len);
	ctx->ev_socket = event_new(ctx->eb, ctx->fd, EV_READ | EV_PERSIST,
							   xqc_client_socket_event_callback, ctx);
	event_add(ctx->ev_socket, NULL);
	return 0;

	err:
	close(ctx->fd);
	return -1;
}

void get_current_time(char *now_time) {
	now_time[0] = '\0';
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
		return;
	}

	// Convert the time to a struct tm and print it with milliseconds
	struct tm *tm_info = gmtime(&ts.tv_sec); // or localtime for local timezone

	if (tm_info == NULL) {
		return;
	}

	sprintf(now_time, "%04d-%02d-%02d %02d:%02d.%03ld",
			tm_info->tm_year + 1900, // Year since 1900
			tm_info->tm_mon + 1,     // Month (0-11)
			tm_info->tm_mday,        // Day of the month (1-31)
			tm_info->tm_hour,        // Hour (0-23)
			tm_info->tm_min,         // Minute (0-59)
			(long) (ts.tv_nsec / 1000000)); // Milliseconds
}

void xqc_client_timer(int fd, short what, void *arg) {
	client_ctx_t *ctx = (client_ctx_t *) arg;
	if (ctx->stream) {
		char buf[128];
		xqc_stream_send(ctx->stream, (unsigned char *) buf, sizeof(buf), 0);
	}

	char now_time[128];
	get_current_time(now_time);
	int64_t speed = ctx->last_recv_bytes;
	speed = speed * 8;
	if (speed >= 1000000) {
		cout << now_time << " speed: " << int(speed / 1000000) << "."
			 << int(speed % 1000000) / 10000
			 << "mbps/s" << endl;
	} else {
		cout << now_time << " speed: " << int(speed / 1000) << "."
			 << int(speed % 1000) / 10 << "kbps/s"
			 << endl;
	}
	ctx->last_recv_bytes = 0;
}

static void xqc_client_engine_callback(int fd, short what, void *arg) {
	client_ctx_t *ctx = (client_ctx_t *) arg;
	xqc_engine_main_logic(ctx->engine);
}

int main(int argc, char *argv[]) {
	const char *ip = "192.168.6.42";
	//const char *ip = "127.0.0.1";
	int port = 9001;

	client_ctx_t *ctx = (client_ctx_t *) malloc(sizeof(client_ctx_t));
	memset(ctx, 0, sizeof(client_ctx_t));
	ctx->eb = event_base_new();
	ctx->ev_engine = event_new(ctx->eb, -1, 0, xqc_client_engine_callback, ctx);

	if (xqc_client_create_socket(ctx) < 0)
		return -1;

	if (xqc_client_create_engine(ctx) < 0)
		return -1;

	if (xqc_client_create_conn(ctx, ip, port) < 0)
		return -1;

	// start speed timer
	ctx->ev_timer = event_new(ctx->eb, -1, EV_PERSIST | EV_TIMEOUT, xqc_client_timer, ctx);
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	event_add(ctx->ev_timer, &tv);

	event_base_dispatch(ctx->eb);

	return 0;
}