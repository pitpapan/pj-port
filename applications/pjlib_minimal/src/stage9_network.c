#include <pjlib.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#define LOOPBACK_ADDRESS "127.0.0.1"

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Stage 9] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Stage 9] FAIL %s:%d condition=%s\n", test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                    \
		pj_status_t check_status_ = (expression);                          \
		if (check_status_ != PJ_SUCCESS)                                   \
			return fail_status((test_name), __LINE__, check_status_);     \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                    \
		if (!(condition))                                                 \
			return fail_value((test_name), __LINE__, #condition);        \
	} while (0)

static int make_loopback_address(pj_sockaddr_in *address, pj_uint16_t port)
{
	pj_str_t host;

	pj_cstr(&host, LOOPBACK_ADDRESS);
	return pj_sockaddr_in_init(address, &host, port);
}

static int test_dns(void)
{
	const char *test = "DNS/resolver";
	pj_str_t numeric;
	pj_str_t localhost;
	pj_str_t missing;
	pj_addrinfo addresses[4];
	pj_hostent hostent;
	unsigned count;
	unsigned i;
	pj_status_t status;

	pj_cstr(&numeric, LOOPBACK_ADDRESS);
	count = PJ_ARRAY_SIZE(addresses);
	CHECK_STATUS(test, pj_getaddrinfo(pj_AF_INET(), &numeric, &count,
					  addresses));
	CHECK_TRUE(test, count > 0);
	CHECK_TRUE(test, addresses[0].ai_addr.addr.sa_family == pj_AF_INET());

	/* Repeat numeric resolution to catch stale/static-result handling. */
	for (i = 0; i < 3; ++i) {
		count = PJ_ARRAY_SIZE(addresses);
		CHECK_STATUS(test, pj_getaddrinfo(pj_AF_INET(), &numeric, &count,
						  addresses));
		CHECK_TRUE(test, count > 0);
	}

	pj_cstr(&localhost, "localhost");
	count = PJ_ARRAY_SIZE(addresses);
	CHECK_STATUS(test, pj_getaddrinfo(pj_AF_INET(), &localhost, &count,
					  addresses));
	CHECK_TRUE(test, count > 0);
	CHECK_STATUS(test, pj_gethostbyname(&localhost, &hostent));
	CHECK_TRUE(test, hostent.h_addr_list != NULL && hostent.h_addr != NULL);

	pj_cstr(&missing, "stage9-name-that-does-not-exist.invalid");
	count = PJ_ARRAY_SIZE(addresses);
	status = pj_getaddrinfo(pj_AF_INET(), &missing, &count, addresses);
	CHECK_TRUE(test, status != PJ_SUCCESS);

	printk("[Stage 9] DNS numeric, localhost, repeated, and failed lookup: PASSED\n");
	return 0;
}

static int test_udp(void)
{
	const char *test = "UDP loopback";
	pj_sock_t receiver = PJ_INVALID_SOCKET;
	pj_sock_t sender = PJ_INVALID_SOCKET;
	pj_sockaddr_in receiver_address;
	pj_sockaddr_in destination;
	pj_sockaddr_in source;
	int address_length;
	pj_ssize_t length;
	char received[32];
	const char payload[] = "stage9-udp";

	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
					  &receiver));
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
					  &sender));
	CHECK_STATUS(test, make_loopback_address(&receiver_address, 0));
	CHECK_STATUS(test, pj_sock_bind(receiver,
					       (pj_sockaddr_t *)&receiver_address,
					       sizeof(receiver_address)));

	address_length = sizeof(receiver_address);
	CHECK_STATUS(test, pj_sock_getsockname(receiver,
						(pj_sockaddr_t *)&receiver_address,
						&address_length));
	CHECK_TRUE(test, pj_sockaddr_in_get_port(&receiver_address) != 0);
	CHECK_STATUS(test, make_loopback_address(&destination,
						 pj_sockaddr_in_get_port(&receiver_address)));
	length = sizeof(payload) - 1;
	CHECK_STATUS(test, pj_sock_sendto(sender, payload, &length, 0,
					 (pj_sockaddr_t *)&destination,
					 sizeof(destination)));
	CHECK_TRUE(test, length == (pj_ssize_t)(sizeof(payload) - 1));

	memset(received, 0, sizeof(received));
	address_length = sizeof(source);
	length = sizeof(received) - 1;
	CHECK_STATUS(test, pj_sock_recvfrom(receiver, received, &length, 0,
					   (pj_sockaddr_t *)&source, &address_length));
	CHECK_TRUE(test, length == (pj_ssize_t)(sizeof(payload) - 1));
	CHECK_TRUE(test, memcmp(received, payload, sizeof(payload) - 1) == 0);
	CHECK_TRUE(test, pj_sockaddr_in_get_port(&source) != 0);
	CHECK_STATUS(test, pj_sock_close(sender));
	CHECK_STATUS(test, pj_sock_close(receiver));
	printk("[Stage 9] UDP bind/send/receive/close: PASSED\n");
	return 0;
}

static int test_nonblocking_and_options(void)
{
	const char *test = "socket options/nonblocking";
	pj_sock_t sock = PJ_INVALID_SOCKET;
	int flags;
	int enabled = 1;
	int option_length = sizeof(enabled);
	pj_ssize_t length;
	char buffer[8];
	pj_status_t status;

	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &sock));
	CHECK_STATUS(test, pj_sock_setsockopt(sock, pj_SOL_SOCKET(),
						 pj_SO_REUSEADDR(), &enabled,
						 sizeof(enabled)));
	CHECK_STATUS(test, pj_sock_getsockopt(sock, pj_SOL_SOCKET(),
						 pj_SO_REUSEADDR(), &enabled,
						 &option_length));
	CHECK_TRUE(test, enabled != 0);

	flags = fcntl((int)sock, F_GETFL, 0);
	CHECK_TRUE(test, flags >= 0);
	CHECK_TRUE(test, fcntl((int)sock, F_SETFL, flags | O_NONBLOCK) == 0);
	length = sizeof(buffer);
	status = pj_sock_recv(sock, buffer, &length, 0);
	CHECK_TRUE(test, status == PJ_STATUS_FROM_OS(EAGAIN) ||
			 status == PJ_STATUS_FROM_OS(EWOULDBLOCK));
	CHECK_TRUE(test, fcntl((int)sock, F_SETFL, flags) == 0);
	CHECK_STATUS(test, pj_sock_close(sock));
	printk("[Stage 9] SO_REUSEADDR and nonblocking EAGAIN mapping: PASSED\n");
	return 0;
}

static int test_tcp(void)
{
	const char *test = "TCP loopback";
	pj_sock_t listener = PJ_INVALID_SOCKET;
	pj_sock_t client = PJ_INVALID_SOCKET;
	pj_sock_t server = PJ_INVALID_SOCKET;
	pj_sock_t probe = PJ_INVALID_SOCKET;
	pj_sock_t probe_server = PJ_INVALID_SOCKET;
	pj_sockaddr_in address;
	pj_sockaddr_in peer;
	int address_length;
	int enabled = 1;
	pj_ssize_t length;
	char received[32];
	const char request[] = "stage9-request";
	const char response[] = "stage9-response";
	pj_bool_t server_closed = PJ_FALSE;

	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0,
					  &listener));
	CHECK_STATUS(test, pj_sock_setsockopt(listener, pj_SOL_SOCKET(),
						 pj_SO_REUSEADDR(), &enabled,
						 sizeof(enabled)));
	CHECK_STATUS(test, make_loopback_address(&address, 0));
	CHECK_STATUS(test, pj_sock_bind(listener, (pj_sockaddr_t *)&address,
					       sizeof(address)));
	address_length = sizeof(address);
	CHECK_STATUS(test, pj_sock_getsockname(listener,
						(pj_sockaddr_t *)&address, &address_length));
	CHECK_STATUS(test, pj_sock_listen(listener, 1));

	/* Validate the BSD backend's nonblocking connect result contract. */
	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0,
					  &probe));
	{
		int flags = fcntl((int)probe, F_GETFL, 0);
		pj_status_t connect_status;

		CHECK_TRUE(test, flags >= 0);
		CHECK_TRUE(test, fcntl((int)probe, F_SETFL, flags | O_NONBLOCK) == 0);
		connect_status = pj_sock_connect(probe, (pj_sockaddr_t *)&address,
						 sizeof(address));
		if (connect_status == PJ_SUCCESS) {
			int probe_length = sizeof(peer);

			CHECK_STATUS(test, pj_sock_accept(listener, &probe_server,
							(pj_sockaddr_t *)&peer, &probe_length));
			CHECK_STATUS(test, pj_sock_close(probe_server));
			probe_server = PJ_INVALID_SOCKET;
			printk("[Stage 9] nonblocking connect completed immediately\n");
		} else {
			CHECK_TRUE(test, connect_status == PJ_STATUS_FROM_OS(EINPROGRESS));
			printk("[Stage 9] nonblocking connect returned EINPROGRESS\n");
		}
	}
	CHECK_STATUS(test, pj_sock_close(probe));
	probe = PJ_INVALID_SOCKET;

	CHECK_STATUS(test, pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0,
					  &client));
	CHECK_STATUS(test, pj_sock_connect(client, (pj_sockaddr_t *)&address,
						 sizeof(address)));
	address_length = sizeof(peer);
	CHECK_STATUS(test, pj_sock_accept(listener, &server,
						(pj_sockaddr_t *)&peer, &address_length));
	CHECK_TRUE(test, server != PJ_INVALID_SOCKET);

	length = sizeof(request) - 1;
	CHECK_STATUS(test, pj_sock_send(client, request, &length, 0));
	CHECK_TRUE(test, length == (pj_ssize_t)(sizeof(request) - 1));
	memset(received, 0, sizeof(received));
	length = sizeof(received) - 1;
	CHECK_STATUS(test, pj_sock_recv(server, received, &length, 0));
	CHECK_TRUE(test, length == (pj_ssize_t)(sizeof(request) - 1));
	CHECK_TRUE(test, memcmp(received, request, sizeof(request) - 1) == 0);

	length = sizeof(response) - 1;
	CHECK_STATUS(test, pj_sock_send(server, response, &length, 0));
	memset(received, 0, sizeof(received));
	length = sizeof(received) - 1;
	CHECK_STATUS(test, pj_sock_recv(client, received, &length, 0));
	CHECK_TRUE(test, length == (pj_ssize_t)(sizeof(response) - 1));
	CHECK_TRUE(test, memcmp(received, response, sizeof(response) - 1) == 0);

	{
		pj_status_t shutdown_status = pj_sock_shutdown(server, PJ_SD_SEND);

		if (shutdown_status == PJ_STATUS_FROM_OS(ENOTSUP) ||
		    shutdown_status == PJ_STATUS_FROM_OS(EOPNOTSUPP)) {
			printk("[Stage 9] socket shutdown is unsupported; validating peer close instead\n");
			CHECK_STATUS(test, pj_sock_close(server));
			server = PJ_INVALID_SOCKET;
			server_closed = PJ_TRUE;
		} else if (shutdown_status != PJ_SUCCESS) {
			return fail_status(test, __LINE__, shutdown_status);
		}
	}
	length = sizeof(received);
	{
		pj_status_t peer_status = pj_sock_recv(client, received, &length, 0);

		CHECK_TRUE(test, (peer_status == PJ_SUCCESS && length == 0) ||
				 peer_status == PJ_STATUS_FROM_OS(ECONNRESET) ||
				 peer_status == PJ_STATUS_FROM_OS(ENOTCONN));
	}
	if (!server_closed)
		CHECK_STATUS(test, pj_sock_close(server));
	CHECK_STATUS(test, pj_sock_close(client));
	CHECK_STATUS(test, pj_sock_close(listener));
	printk("[Stage 9] TCP bind/listen/connect/accept/send/receive/peer-close: PASSED\n");
	return 0;
}

static int test_tcp_peer_names(void)
{
    const char *test = "TCP peer/local names";

    pj_sock_t listener = PJ_INVALID_SOCKET;
    pj_sock_t client = PJ_INVALID_SOCKET;
    pj_sock_t server = PJ_INVALID_SOCKET;

    pj_sockaddr_in addr;
    pj_sockaddr_in peer;
    pj_sockaddr_in local;

    int len;

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, &listener));

    CHECK_STATUS(test, make_loopback_address(&addr, 0));

    CHECK_STATUS(test,
        pj_sock_bind(listener,
                     (pj_sockaddr_t *)&addr,
                     sizeof(addr)));

    len = sizeof(addr);

    CHECK_STATUS(test,
        pj_sock_getsockname(listener,
                            (pj_sockaddr_t *)&addr,
                            &len));

    CHECK_STATUS(test, pj_sock_listen(listener, 1));

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, &client));

    CHECK_STATUS(test,
        pj_sock_connect(client,
                        (pj_sockaddr_t *)&addr,
                        sizeof(addr)));

    len = sizeof(peer);

    CHECK_STATUS(test,
        pj_sock_accept(listener,
                       &server,
                       (pj_sockaddr_t *)&peer,
                       &len));

    len = sizeof(local);

    CHECK_STATUS(test,
        pj_sock_getsockname(client,
                            (pj_sockaddr_t *)&local,
                            &len));

    CHECK_TRUE(test,
        pj_sockaddr_in_get_port(&local) != 0);

    len = sizeof(peer);

    CHECK_STATUS(test,
        pj_sock_getpeername(client,
                            (pj_sockaddr_t *)&peer,
                            &len));

    CHECK_TRUE(test,
        pj_sockaddr_in_get_port(&peer) ==
        pj_sockaddr_in_get_port(&addr));

    CHECK_STATUS(test, pj_sock_close(server));
    CHECK_STATUS(test, pj_sock_close(client));
    CHECK_STATUS(test, pj_sock_close(listener));

    printk("[Stage 9] TCP getpeername/getsockname: PASSED\n");
    return 0;
}


static int test_connection_refused(void)
{
    const char *test = "TCP connection refused";

    pj_sock_t sock = PJ_INVALID_SOCKET;
    pj_sockaddr_in addr;
    pj_status_t status;

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, &sock));

    CHECK_STATUS(test,
        make_loopback_address(&addr, 65000));

    status = pj_sock_connect(
        sock,
        (pj_sockaddr_t *)&addr,
        sizeof(addr));

    CHECK_TRUE(test, status != PJ_SUCCESS);

    CHECK_STATUS(test, pj_sock_close(sock));

    printk("[Stage 9] TCP connection-refused path: PASSED\n");
    return 0;
}

static int test_nonblocking_accept(void)
{
    const char *test = "nonblocking accept";

    pj_sock_t listener = PJ_INVALID_SOCKET;
    pj_sockaddr_in addr;

    int len;
    int flags;

    pj_sock_t accepted;
    pj_status_t status;

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(),
                       pj_SOCK_STREAM(),
                       0,
                       &listener));

    CHECK_STATUS(test,
        make_loopback_address(&addr, 0));

    CHECK_STATUS(test,
        pj_sock_bind(listener,
                     (pj_sockaddr_t *)&addr,
                     sizeof(addr)));

    len = sizeof(addr);

    CHECK_STATUS(test,
        pj_sock_getsockname(listener,
                            (pj_sockaddr_t *)&addr,
                            &len));

    CHECK_STATUS(test,
        pj_sock_listen(listener, 1));

    flags = fcntl((int)listener, F_GETFL, 0);

    CHECK_TRUE(test, flags >= 0);

    CHECK_TRUE(test,
        fcntl((int)listener,
              F_SETFL,
              flags | O_NONBLOCK) == 0);

    len = sizeof(addr);

    status = pj_sock_accept(
        listener,
        &accepted,
        (pj_sockaddr_t *)&addr,
        &len);

    CHECK_TRUE(test,
        status == PJ_STATUS_FROM_OS(EAGAIN) ||
        status == PJ_STATUS_FROM_OS(EWOULDBLOCK));

    CHECK_STATUS(test,
        pj_sock_close(listener));

    printk("[Stage 9] nonblocking accept: PASSED\n");
    return 0;
}

static int test_udp_large_payload(void)
{
    const char *test = "UDP large payload";

    pj_sock_t rx = PJ_INVALID_SOCKET;
    pj_sock_t tx = PJ_INVALID_SOCKET;

    pj_sockaddr_in rx_addr;
    pj_sockaddr_in src;

    char send_buf[1400];
    char recv_buf[1400];

    int addrlen;
    pj_ssize_t len;

    memset(send_buf, 0x5a, sizeof(send_buf));

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(),
                       pj_SOCK_DGRAM(),
                       0,
                       &rx));

    CHECK_STATUS(test,
        pj_sock_socket(pj_AF_INET(),
                       pj_SOCK_DGRAM(),
                       0,
                       &tx));

    CHECK_STATUS(test,
        make_loopback_address(&rx_addr, 0));

    CHECK_STATUS(test,
        pj_sock_bind(rx,
                     (pj_sockaddr_t *)&rx_addr,
                     sizeof(rx_addr)));

    addrlen = sizeof(rx_addr);

    CHECK_STATUS(test,
        pj_sock_getsockname(rx,
                            (pj_sockaddr_t *)&rx_addr,
                            &addrlen));

    len = sizeof(send_buf);

    CHECK_STATUS(test,
        pj_sock_sendto(tx,
                       send_buf,
                       &len,
                       0,
                       (pj_sockaddr_t *)&rx_addr,
                       sizeof(rx_addr)));

    len = sizeof(recv_buf);
    addrlen = sizeof(src);

    CHECK_STATUS(test,
        pj_sock_recvfrom(rx,
                         recv_buf,
                         &len,
                         0,
                         (pj_sockaddr_t *)&src,
                         &addrlen));

    CHECK_TRUE(test, len == sizeof(send_buf));

    CHECK_TRUE(test,
        memcmp(send_buf,
               recv_buf,
               sizeof(send_buf)) == 0);

    CHECK_STATUS(test, pj_sock_close(tx));
    CHECK_STATUS(test, pj_sock_close(rx));

    printk("[Stage 9] UDP large payload: PASSED\n");
    return 0;
}

int stage9_network_run(void)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;

	printk("[Stage 9] basic networking validation (existing BSD/POSIX backends)\n");
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);

	pj_caching_pool_init(&caching_pool, NULL, 0);
	pool = pj_pool_create(&caching_pool.factory, "stage9", 8192, 8192, NULL);
	if (pool == NULL) {
		fail_value("pool setup", __LINE__, "pool != NULL");
		pj_caching_pool_destroy(&caching_pool);
		pj_shutdown();
		return 1;
	}

if (test_dns() != 0 ||
    test_udp() != 0 ||
    test_udp_large_payload() != 0 ||
    test_nonblocking_and_options() != 0 ||
    test_nonblocking_accept() != 0 ||
    test_tcp() != 0 ||
    test_tcp_peer_names() != 0 ||
    test_connection_refused() != 0) {
		pj_pool_release(pool);
		pj_caching_pool_destroy(&caching_pool);
		pj_shutdown();
		return 1;
	}

	pj_pool_release(pool);
	pj_caching_pool_destroy(&caching_pool);
	pj_shutdown();
	printk("STAGE 9 RESULT: PASSED\n");
	return 0;
}