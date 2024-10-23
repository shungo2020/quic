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

typedef struct server_ctx_s {
	xqc_engine_t *engine;
	struct event_base *eb;
	struct event *ev_engine;

	int fd;
	struct event *ev_socket;
	struct sockaddr local_addr;
	socklen_t local_addr_len;
} server_ctx_t;

typedef struct conn_ctx_s {
	server_ctx_s *server_ctx;
	const xqc_cid_t *cid;
	xqc_stream_t *stream;
	struct sockaddr remote_addr;
	socklen_t remote_addr_len;
} conn_ctx_t;

void xqc_log(xqc_log_level_t lvl, const void *buf, size_t count, void *engine_user_data)
{
	cout << (char*)buf << endl;
}

void print_address(const struct sockaddr *addr) {
    if (addr == NULL) {
        printf("Invalid address pointer\n");
        return;
    }

    switch (addr->sa_family) {
        case AF_INET: {  // IPv4
            struct sockaddr_in *ipv4_addr = (struct sockaddr_in *)addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(ipv4_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
            printf("IPv4 Address: %s Port: %d\n", ip_str, ntohs(ipv4_addr->sin_port));
            break;
        }
        case AF_INET6: {  // IPv6
            struct sockaddr_in6 *ipv6_addr = (struct sockaddr_in6 *)addr;
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &(ipv6_addr->sin6_addr), ip_str, INET6_ADDRSTRLEN);
            printf("IPv6 Address: %s Port: %d\n", ip_str, ntohs(ipv6_addr->sin6_port));
            break;
        }
        default:
            printf("Unknown address family\n");
    }
}

void xqc_server_set_event_timer(xqc_usec_t wake_after, void *user_data) {
	server_ctx_t *ctx = (server_ctx_t *) user_data;
	struct timeval tv;
	tv.tv_sec = wake_after / 1000000;
	tv.tv_usec = wake_after % 1000000;
	event_add(ctx->ev_engine, &tv);
}

static void xqc_server_engine_callback(int fd, short what, void *arg) {
	server_ctx_t *ctx = (server_ctx_t *) arg;
	xqc_engine_main_logic(ctx->engine);
}

void xqc_keylog_cb(const xqc_cid_t *scid, const char *line, void *user_data) {}

void xqc_server_socket_event_callback(int fd, short what, void *arg) {
	server_ctx_t *ctx = (server_ctx_t *) arg;
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

			//print_address(&peer_addr);
			if (xqc_engine_packet_process(ctx->engine, (const unsigned char *) buffer, recv_size,
									  &ctx->local_addr, ctx->local_addr_len,
									  &peer_addr, peer_addrlen,
									  xqc_now(), ctx) != XQC_OK) {
				return;
			} 
		}

		xqc_engine_finish_recv(ctx->engine);
	}
}

int xqc_server_accept(xqc_engine_t *engine, xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data) {
	cout << "conn accpet" << endl;
	server_ctx_t *ctx = (server_ctx_t *)user_data;
	conn_ctx_t * conn_ctx = (conn_ctx_t *)malloc(sizeof(conn_ctx_t));
	memset((char*)conn_ctx, 0, sizeof(conn_ctx_t));
	conn_ctx->server_ctx = ctx;
	conn_ctx->cid = cid;
	xqc_conn_set_transport_user_data(conn, conn_ctx);
	return 0;
}

ssize_t xqc_server_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
						void *user) {
	conn_ctx_t *ctx = (conn_ctx_t *)user;
	//print_address(peer_addr);
	//cout << "fd:" << ctx->server_ctx->fd << endl;
	ssize_t res = sendto(ctx->server_ctx->fd, buf, size, 0, peer_addr, peer_addrlen);
	if (res < 0) {
		//cout << "xqc_client_write_socket err" << (int)res << " err:" << strerror(get_sys_errno()) << endl;
		if (get_sys_errno() == EAGAIN) {
			res = XQC_SOCKET_EAGAIN;
		} else {
			res = XQC_SOCKET_ERROR;
		}
	}
	return res;
}

void xqc_server_conn_update_cid_notify(xqc_connection_t *conn, const xqc_cid_t *retire_cid, const xqc_cid_t *new_cid,
									   void *user_data) {
}

int xqc_server_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data) {
	cout << "conn close" << endl;
	conn_ctx_t *ctx = (conn_ctx_t *)user_data;
	xqc_conn_close(ctx->server_ctx->engine, ctx->cid);
	free(ctx);
	return 0;
}

void xqc_server_conn_handshake_finished(xqc_connection_t *conn, void *user_data, void *conn_proto_data) {
	cout << "conn handshake" << endl;
}

int xqc_server_stream_read_notify(xqc_stream_t *stream, void *user_data) {
	//conn_ctx_t *ctx = (conn_ctx_t *)user_data;
	char buffer[4096];
	while (1)
	{
		uint8_t fin = 0;
		ssize_t s = xqc_stream_recv(stream, (unsigned char*)buffer, sizeof(buffer), &fin);
		if (s <= 0)
			break;
	}
	return 0;
}

void xqc_send_full(xqc_stream_t *stream)
{
	int total = 0;
	while (1)
	{
		char buffer[4096];
		ssize_t s = xqc_stream_send(stream, (unsigned char*)buffer, sizeof(buffer), 0);
		if (s <= 0)
			break;
		total += s;
	}
	//cout << "total:" << total << endl;
}

int xqc_server_stream_write_notify(xqc_stream_t *stream, void *user_data) {
	//cout << "===" << endl;
	//conn_ctx_t *ctx = (conn_ctx_t *)user_data;
	xqc_send_full(stream);
	return 0;
}

int xqc_server_stream_create_notify(xqc_stream_t *stream, void *user_data) {
	cout << "new stream" << endl;
	conn_ctx_t* ctx = (conn_ctx_t*)xqc_get_conn_user_data_by_stream(stream);
	ctx->stream = stream;
	xqc_stream_set_user_data(stream, ctx);
	xqc_send_full(stream);
	return 0;
}

int xqc_server_stream_close_notify(xqc_stream_t *stream, void *user_data) {
	cout << "stream close" << endl;
	return 0;
}

int xqc_server_create_socket(server_ctx_t *ctx) {
	int optval;
	struct sockaddr *saddr = (struct sockaddr *)&ctx->local_addr;
	struct sockaddr_in *addr_v4 = (struct sockaddr_in *)saddr;
	ctx->local_addr_len = sizeof(struct sockaddr);
	ctx->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->fd < 0) {
		cout << "create socket failed, errno" << endl;
		return -1;
	}
	if (fcntl(ctx->fd, F_SETFL, O_NONBLOCK) == -1) {
		cout << "set socket nonblock failed" << endl;
		goto err;
	}

	optval = 1;
	if (setsockopt(ctx->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
		cout << "setsockopt failed, errno: " << get_sys_errno() << endl;
		goto err;
	}

	memset(saddr, 0, sizeof(struct sockaddr_in));
	addr_v4->sin_family = AF_INET;
	addr_v4->sin_port = htons(9001);
	addr_v4->sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(ctx->fd, saddr, ctx->local_addr_len) < 0) {
		cout << "bind socket failed, errno: " << get_sys_errno() << endl;
		goto err;
	}

	//cout << "==fd:" << ctx->fd << endl;
	ctx->ev_socket = event_new(ctx->eb, ctx->fd, EV_READ | EV_PERSIST,
							   xqc_server_socket_event_callback, ctx);
	event_add(ctx->ev_socket, NULL);
	return 0;

	err:
	close(ctx->fd);
	return -1;
}

int xqc_server_create_engine(server_ctx_t *ctx, const char *priv_key_file, const char *cert_file) {
	xqc_engine_ssl_config_t ssl_cfg;
	memset((char *) &ssl_cfg, 0, sizeof(ssl_cfg));
	ssl_cfg.private_key_file = (char*)priv_key_file;
	ssl_cfg.cert_file =  (char*)cert_file;
	ssl_cfg.ciphers = (char *) XQC_TLS_CIPHERS;
	ssl_cfg.groups = (char *) XQC_TLS_GROUPS;

	/* init engine callbacks */
	xqc_engine_callback_t callback = {
			.set_event_timer = xqc_server_set_event_timer, /* call xqc_engine_main_logic when the timer expires */
			.log_callbacks = {
            	.xqc_log_write_err = xqc_log,
            	.xqc_log_write_stat = xqc_log,
        	},
			.keylog_cb = xqc_keylog_cb,
	};

	xqc_transport_callbacks_t transport_cbs = {
			.server_accept = xqc_server_accept,
			.write_socket = xqc_server_write_socket,
			.conn_update_cid_notify = xqc_server_conn_update_cid_notify,
	};

	xqc_config_t config;
	if (xqc_engine_get_default_config(&config, XQC_ENGINE_SERVER)) {
		cout << "get default xquic server cfg error" << endl;
		return -1;
	}
	//config.cfg_log_level = log_level;

	ctx->engine = xqc_engine_create(XQC_ENGINE_SERVER, &config, &ssl_cfg, &callback, &transport_cbs, ctx);
	if (ctx->engine == NULL) {
		cout << "xqc_engine_create error" << endl;
		return -1;
	}
	cout << "xqc server engine_create ok" << endl;
	/* register transport callbacks */
	xqc_app_proto_callbacks_t ap_cbs = {
			.conn_cbs = {
					.conn_close_notify = xqc_server_conn_close_notify,
					.conn_handshake_finished = xqc_server_conn_handshake_finished,
			},
			.stream_cbs = {
					.stream_read_notify = xqc_server_stream_read_notify,
					.stream_write_notify = xqc_server_stream_write_notify,
					.stream_create_notify = xqc_server_stream_create_notify,
					.stream_close_notify = xqc_server_stream_close_notify,
			},
	};
	xqc_engine_register_alpn(ctx->engine, XQC_ALPN_TRANSPORT, 9, &ap_cbs, NULL);

	uint32_t cong_flags = XQC_BBR_FLAG_NONE;
	xqc_conn_settings_t coon_setting = {
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
	xqc_server_set_conn_settings(ctx->engine, &coon_setting);
	return 0;
}

int main(int argc, char *argv[]) {
	server_ctx_t *ctx = (server_ctx_t *) malloc(sizeof(server_ctx_t));
	memset(ctx, 0, sizeof(server_ctx_t));
	ctx->eb = event_base_new();
	ctx->ev_engine = event_new(ctx->eb, -1, 0, xqc_server_engine_callback, ctx);
	if (xqc_server_create_socket(ctx) < 0)
		return -1;

	if (xqc_server_create_engine(ctx, "private_key.pem", "certificate.crt") < 0)
		return -1;

	event_base_dispatch(ctx->eb);
	return 0;
}