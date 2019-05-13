#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>

#define MAX_CONNECTIONS 1024
#define BUFFER_SIZE 8192

static struct {
	int verbose;
	char *listen;
	struct sockaddr_in listen_addr;
	int listen_sock;
	char *connect;
	struct sockaddr_in connect_addr;
} g_config;

struct sock_buffer {
	int fd;
	size_t len;
	int top, tail;
	uint8_t data[BUFFER_SIZE];
};

static void
print_help(int argc, char **argv) {
	printf("Usage: %s -l <input addr> -c <output addr> [-v]\n"
			"\n"
			"  -l, --listen <address>\n"
			"  -c, --connect <address>\n"
			"  -v, --verbose\n",
			argv[0]);
}

static int
parse_addr(char *url, struct sockaddr_in *addr) {
	char host[1024];
	unsigned short port;
	int n;

	n = sscanf(url, "%99[^:]:%hu", host, &port);
	printf("\"%s\": host: %s, port: %d, n = %d\n", url, host, port, n);

	/* TODO: IPv6? */
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	if(inet_pton(AF_INET, host, &addr->sin_addr) <= 0) {
		printf("Error: Cannot parse address\n");
		return -1;
	}

	return 0;
}

static int
parse_args(int argc, char **argv) {
	int c;
	int option_index = 0;

	static struct option long_options[] = {
		{ "listen", required_argument, 0, 'l' },
		{ "connect", required_argument, 0, 'c' },
		{ "verbose", no_argument, 0, 'v' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	do {
		c = getopt_long(argc, argv, "l:c:hv", NULL, &option_index);
		switch (c) {
			case 'l':
				g_config.listen = optarg;
				parse_addr(g_config.listen,
						&g_config.listen_addr);
				break;
			case 'c':
				g_config.connect = optarg;
				parse_addr(g_config.connect,
						&g_config.connect_addr);
				break;
			case 'h':
				print_help(argc, argv);
				break;
			case 'v':
				g_config.verbose = 1;
				break;
			default:
				break;
		}
	} while (c != -1);

	if (g_config.listen == NULL) {
		printf("Error: source not defined\n");
		return -EINVAL;
	}

	if (g_config.connect == NULL) {
		printf("Error: destination not defined\n");
		return -EINVAL;
	}

	return 0;
}

static int
sock_copy(int destination, int source, int direction) {
	uint8_t buf[1024*8];
	int n_recv, n_sent;
	int n_expected = 0;
	int i;

	n_recv = recv(source, buf, sizeof(buf), 0);
	if (n_recv <= 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			printf("Error: recv from source: %s\n", strerror(errno));
			return -1;
		}
		return 0;
	}
#if 1
	if (buf[0] == 0x23) {
		int segment_size = ((int)buf[5] << 16) +
			((int)buf[6] << 8) + (int)buf[7];
		n_expected = segment_size;
		printf("ISCSI Login Response (DataSegmentSize=%d, %d):\n",
				segment_size, n_expected);
	} else if (buf[0] == 0x25) {
		int segment_size = ((int)buf[5] << 16) +
			((int)buf[6] << 8) + (int)buf[7];
		printf("SCSI Data In Lun (DataSegmentSize=%d):\n",
				segment_size);
		n_expected = segment_size;
	} else if (buf[0] == 0x01) { /* READ(10) */
		int expected_data_transfer_length = ((int)buf[20] << 24) +
				((int)buf[21] << 16) + ((int)buf[22] << 8) +
				((int)buf[23] << 0);
		printf("SCSI READ(10): %d\n", expected_data_transfer_length);
		// n_expected = expected_data_transfer_length;
	}
	while (n_recv < n_expected) {
		printf("n_recv=%d\n", n_recv);
		n_recv += recv(source, buf + n_recv, sizeof(buf), 0);
	}
#endif /* 0 */

	if (g_config.verbose == 1) {
		printf(" %s transmiting %d bytes:\n",
				direction == 0 ? "-->" : "<--", n_recv);
		for (i = 0; i < n_recv; i++) {
			printf("%02x ", buf[i]);
		}
		printf("\n");
	}

	n_sent = send(destination, buf, n_recv, MSG_NOSIGNAL);
	if (n_sent < 0) {
		printf("Error: send to destination: %s\n",
				strerror(errno));
		return -1;
	} else if (n_sent != n_recv) {
		/* FIXIT: It should be handled with somee buffer */
		printf("Error: n_recv != n_sent\n");
		return -1;
	}

	return 0;
}

static int
sock_set_flags(int fd, int flags) {
	int old_flags;

	old_flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		printf("Error: Cannot get socket flags: %s\n", strerror(errno));
		return -1;
	}
	if (fcntl(fd, F_SETFL, old_flags | flags) < 0) {
		printf("Error: Cannot set socket flags: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/**
 * TODO: This thread should be replaced with select mechanism
 */
static void *
connect_thread_fn(void *args) {
	int rv, er;
	int source_sock = (intptr_t)args;
	int connect_sock = socket(AF_INET, SOCK_STREAM, 0);
	fd_set read_fds, write_fds;
	int n_fds;

	printf("Connecting...\n");
	rv = connect(connect_sock, (struct sockaddr *)&g_config.connect_addr,
			sizeof(g_config.connect_addr));
	if (rv < 0) {
		printf("Error: Connect failed: %s\n", strerror(errno));
		goto error;
	}

#if 0
	if (sock_set_flags(source_sock, O_NONBLOCK) < 0) {
		goto error;
	}
	if (sock_set_flags(connect_sock, O_NONBLOCK) < 0) {
		goto error;
	}
#endif /* 0 */

	n_fds = (source_sock > connect_sock ? source_sock : connect_sock) + 1;

	while (1) {

		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);

		FD_SET(source_sock, &read_fds);
		FD_SET(connect_sock, &read_fds);

		rv = select(n_fds, &read_fds, &write_fds, NULL, NULL);
		if (rv == -1) {
			perror("select()");
			goto error;
		} else if (rv == 0) {
			/* Timeout -- not implemented yet */
		}

		if (FD_ISSET(connect_sock, &read_fds)) {
			if (sock_copy(source_sock, connect_sock, 0) < 0) {
				goto error;
			}
		}
		if (FD_ISSET(source_sock, &read_fds)) {
			if (sock_copy(connect_sock, source_sock, 1) < 0) {
				goto error;
			}
		}
	}

	return NULL;

error:
	printf("Closing connection\n");
	close(source_sock);
	close(connect_sock);
	pthread_exit(NULL);
	return NULL;
}

static int
sock_listen(void) {
	int connect_sock = 0;
	pthread_t connect_thread;
	int rv;

	g_config.listen_sock = socket(AF_INET, SOCK_STREAM, 0);

	rv = bind(g_config.listen_sock, (struct sockaddr*)&g_config.listen_addr,
			sizeof(g_config.listen_addr));
	if (rv < 0) {
		printf("Error: Binding: %s\n", strerror(errno));
		return -1;
	}

	rv = listen(g_config.listen_sock, 10);
	if (rv < 0) {
		printf("Error: Listen: %s\n", strerror(errno));
		return -1;
	}

	printf("Waiting for connection...\n");
	while (1) {
		connect_sock = accept(g_config.listen_sock,
				(struct sockaddr*)NULL, NULL);
		pthread_create(&connect_thread, NULL, connect_thread_fn,
				(void *)(intptr_t)connect_sock);
	};

	return 0;
}

int
main(int argc, char **argv) {

	if (parse_args(argc, argv) < 0) {
		return -1;
	}

	sock_listen();

	return 0;
}

