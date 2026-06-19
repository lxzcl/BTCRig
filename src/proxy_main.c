#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <jansson.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define BTC_ACCESS(path, mode) _access((path), (mode))
#define BTC_R_OK 4
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define BTC_ACCESS(path, mode) access((path), (mode))
#define BTC_R_OK R_OK
#endif

#define BUFFER_SIZE 32768
#define LISTEN_TCP 0
#define LISTEN_TLS 1
#define LISTEN_AUTO 2

static char stdout_buffer[1024 * 1024];
static char stderr_buffer[64 * 1024];

typedef struct {
    char host[256];
    char port[16];
    int use_tls;
} endpoint_t;

typedef struct {
    int fd;
    int use_tls;
    SSL_CTX *ctx;
    SSL *ssl;
} stream_t;

typedef struct {
    unsigned char data[BUFFER_SIZE];
    size_t off;
    size_t len;
} buffer_t;

typedef struct {
    char listen_host[256];
    char listen_port[16];
    int listen_mode;
    char data_dir[512];
    char cert_path[512];
    char key_path[512];
    endpoint_t upstream;
    int upstream_verify;
    SSL_CTX *server_ctx;
} proxy_config_t;

typedef struct {
    proxy_config_t *config;
    int fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} client_arg_t;

static int network_init(void) {
#if defined(_WIN32)
    static int initialized = 0;
    static int failed = 0;
    if (initialized) {
        return failed ? -1 : 0;
    }

    WSADATA data;
    int rc = WSAStartup(MAKEWORD(2, 2), &data);
    initialized = 1;
    failed = rc != 0;
    if (failed) {
        fprintf(stderr, "[PROXY] WSAStartup failed: %d\n", rc);
        return -1;
    }
#endif
    return 0;
}

#if defined(_WIN32)
static DWORD WINAPI stdio_flush_thread(LPVOID ignored);
#endif

static void configure_stdio(void) {
#if defined(_WIN32)
    setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));
    setvbuf(stderr, stderr_buffer, _IOFBF, sizeof(stderr_buffer));
    HANDLE thread = CreateThread(NULL, 0, stdio_flush_thread, NULL, 0, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
    }
#else
    setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
    setvbuf(stderr, stderr_buffer, _IOLBF, sizeof(stderr_buffer));
#endif
}

#if defined(_WIN32)
static DWORD WINAPI stdio_flush_thread(LPVOID ignored) {
    (void)ignored;
    for (;;) {
        fflush(stdout);
        fflush(stderr);
        Sleep(30);
    }
    return 0;
}
#endif

static void socket_close_fd(int fd) {
    if (fd < 0) {
        return;
    }
#if defined(_WIN32)
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

static int socket_would_block_or_interrupted(void) {
#if defined(_WIN32)
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
    size_t len = 0;

    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int make_directory(const char *path) {
#if defined(_WIN32)
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return 0;
    }
#else
    if (mkdir(path, S_IRUSR | S_IWUSR | S_IXUSR) == 0 || errno == EEXIST) {
        return 0;
    }
#endif
    return -1;
}

static int ensure_directory(const char *path) {
    char tmp[512];
    size_t len = 0;

    copy_text(tmp, sizeof(tmp), path);
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }

    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] != '/' && tmp[i] != '\\') {
            continue;
        }
        if (i == 2 && tmp[1] == ':') {
            continue;
        }
        char saved = tmp[i];
        tmp[i] = '\0';
        if (tmp[0] != '\0' && make_directory(tmp) != 0) {
            return -1;
        }
        tmp[i] = saved;
    }
    return make_directory(tmp);
}

static void join_path(char *out, size_t out_size, const char *dir, const char *name) {
    size_t len = strlen(dir);
    const char *sep = len > 0 && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" :
#if defined(_WIN32)
                      "\\";
#else
                      "/";
#endif
    snprintf(out, out_size, "%s%s%s", dir, sep, name);
}

static void set_default_data_dir(char *out, size_t out_size) {
    const char *configured = getenv("BTC_PROXY_DATA_DIR");

    if (configured != NULL && configured[0] != '\0') {
        copy_text(out, out_size, configured);
        return;
    }
    copy_text(out, out_size, ".");
}

static int parse_host_port(const char *text, char *host, size_t host_size, char *port, size_t port_size) {
    char tmp[320];
    char *colon = NULL;

    if (text == NULL || *text == '\0') {
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s", text);
    colon = strrchr(tmp, ':');
    if (colon == NULL) {
        return -1;
    }
    *colon = '\0';
    copy_text(host, host_size, tmp[0] != '\0' ? tmp : "0.0.0.0");
    copy_text(port, port_size, colon + 1);
    return host[0] == '\0' || port[0] == '\0' ? -1 : 0;
}

static int parse_endpoint(const char *url, endpoint_t *endpoint) {
    const char *p = url;
    char hostport[320];
    char *slash = NULL;
    char *colon = NULL;

    memset(endpoint, 0, sizeof(*endpoint));

    if (strncmp(p, "stratum+tcp://", 14) == 0) {
        p += 14;
    } else if (strncmp(p, "stratum+tls://", 14) == 0 || strncmp(p, "stratum+ssl://", 14) == 0) {
        endpoint->use_tls = 1;
        p += 14;
    } else if (strncmp(p, "tcp://", 6) == 0) {
        p += 6;
    } else if (strncmp(p, "tls://", 6) == 0 || strncmp(p, "ssl://", 6) == 0) {
        endpoint->use_tls = 1;
        p += 6;
    }

    if (*p == '\0') {
        return -1;
    }

    snprintf(hostport, sizeof(hostport), "%s", p);
    slash = strchr(hostport, '/');
    if (slash != NULL) {
        *slash = '\0';
    }

    colon = strrchr(hostport, ':');
    if (colon == NULL) {
        copy_text(endpoint->host, sizeof(endpoint->host), hostport);
        copy_text(endpoint->port, sizeof(endpoint->port), endpoint->use_tls ? "4333" : "3333");
    } else {
        *colon = '\0';
        copy_text(endpoint->host, sizeof(endpoint->host), hostport);
        copy_text(endpoint->port, sizeof(endpoint->port), colon + 1);
    }

    return endpoint->host[0] == '\0' || endpoint->port[0] == '\0' ? -1 : 0;
}

static int set_nonblocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int connect_tcp(const endpoint_t *endpoint) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    if (network_init() != 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(endpoint->host, endpoint->port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "[PROXY] getaddrinfo %s:%s failed: %s\n",
                endpoint->host, endpoint->port, gai_strerror(rc));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        socket_close_fd(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int create_listener(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    if (network_init() != 0) {
        return -1;
    }
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "[PROXY] listen getaddrinfo failed: %s\n", gai_strerror(rc));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, 128) == 0) {
            break;
        }
        socket_close_fd(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static void stream_close(stream_t *stream) {
    if (stream == NULL) {
        return;
    }
    if (stream->ssl != NULL) {
        SSL_shutdown(stream->ssl);
        SSL_free(stream->ssl);
        stream->ssl = NULL;
    }
    if (stream->ctx != NULL) {
        SSL_CTX_free(stream->ctx);
        stream->ctx = NULL;
    }
    if (stream->fd >= 0) {
        socket_close_fd(stream->fd);
        stream->fd = -1;
    }
}

static int stream_open_upstream(stream_t *stream, const endpoint_t *endpoint, int verify) {
    memset(stream, 0, sizeof(*stream));
    stream->fd = -1;
    stream->use_tls = endpoint->use_tls;
    stream->fd = connect_tcp(endpoint);
    if (stream->fd < 0) {
        return -1;
    }

    if (!endpoint->use_tls) {
        return 0;
    }

    stream->ctx = SSL_CTX_new(TLS_client_method());
    if (stream->ctx == NULL) {
        fprintf(stderr, "[PROXY] upstream SSL_CTX_new failed\n");
        stream_close(stream);
        return -1;
    }
    if (verify) {
        if (SSL_CTX_set_default_verify_paths(stream->ctx) != 1) {
            fprintf(stderr, "[PROXY] upstream CA paths unavailable\n");
        }
        SSL_CTX_set_verify(stream->ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(stream->ctx, SSL_VERIFY_NONE, NULL);
    }

    stream->ssl = SSL_new(stream->ctx);
    if (stream->ssl == NULL) {
        fprintf(stderr, "[PROXY] upstream SSL_new failed\n");
        stream_close(stream);
        return -1;
    }
    (void)SSL_set_tlsext_host_name(stream->ssl, endpoint->host);
    if (verify) {
        (void)SSL_set1_host(stream->ssl, endpoint->host);
    }
    SSL_set_fd(stream->ssl, stream->fd);
    if (SSL_connect(stream->ssl) != 1) {
        unsigned long err = ERR_get_error();
        fprintf(stderr, "[PROXY] upstream TLS handshake failed: %s\n",
                err != 0 ? ERR_error_string(err, NULL) : "unknown");
        stream_close(stream);
        return -1;
    }
    SSL_set_mode(stream->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return 0;
}

static void configure_compat_tls_server(SSL_CTX *ctx) {
    if (ctx == NULL) {
        return;
    }

#if defined(TLS1_VERSION)
    (void)SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);
#endif
#if defined(TLS1_3_VERSION)
    (void)SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
#endif
#if defined(SSL_OP_NO_SSLv2)
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);
#endif
#if defined(SSL_OP_NO_SSLv3)
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
#endif
#if defined(SSL_OP_NO_COMPRESSION)
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
#endif

    if (SSL_CTX_set_cipher_list(ctx, "DEFAULT:@SECLEVEL=0") != 1) {
        (void)SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5");
    }
#if defined(TLS1_3_VERSION)
    (void)SSL_CTX_set_ciphersuites(ctx,
                                   "TLS_AES_256_GCM_SHA384:"
                                   "TLS_AES_128_GCM_SHA256:"
                                   "TLS_CHACHA20_POLY1305_SHA256");
#endif
}

static int client_uses_tls(int fd, int listen_mode) {
    unsigned char first_byte = 0;
    int n = 0;

    if (listen_mode == LISTEN_TLS) {
        return 1;
    }
    if (listen_mode == LISTEN_TCP) {
        return 0;
    }

    n = recv(fd, (char *)&first_byte, 1, MSG_PEEK);
    if (n <= 0) {
        return -1;
    }
    return first_byte == 0x16;
}

static int stream_open_client(stream_t *stream, int fd, proxy_config_t *config) {
    int use_tls = client_uses_tls(fd, config->listen_mode);

    memset(stream, 0, sizeof(*stream));
    stream->fd = fd;
    if (use_tls < 0) {
        stream_close(stream);
        return -1;
    }
    stream->use_tls = use_tls;

    if (!stream->use_tls) {
        return 0;
    }

    stream->ssl = SSL_new(config->server_ctx);
    if (stream->ssl == NULL) {
        fprintf(stderr, "[PROXY] client SSL_new failed\n");
        stream_close(stream);
        return -1;
    }
    SSL_set_fd(stream->ssl, stream->fd);
    if (SSL_accept(stream->ssl) != 1) {
        unsigned long err = ERR_get_error();
        fprintf(stderr, "[PROXY] client TLS handshake failed: %s\n",
                err != 0 ? ERR_error_string(err, NULL) : "unknown");
        stream_close(stream);
        return -1;
    }
    SSL_set_mode(stream->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return 0;
}

static int stream_read(stream_t *stream, unsigned char *buf, size_t size) {
    if (stream->use_tls) {
        int n = SSL_read(stream->ssl, buf, (int)size);
        if (n > 0) {
            return n;
        }
        int err = SSL_get_error(stream->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return -2;
        }
        return -1;
    }

    int n = recv(stream->fd, (char *)buf, (int)size, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        return 0;
    }
    return socket_would_block_or_interrupted() ? -2 : -1;
}

static int stream_write(stream_t *stream, const unsigned char *buf, size_t size) {
    if (stream->use_tls) {
        int n = SSL_write(stream->ssl, buf, (int)size);
        if (n > 0) {
            return n;
        }
        int err = SSL_get_error(stream->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return -2;
        }
        return -1;
    }

    int n = send(stream->fd, (const char *)buf, (int)size, 0);
    if (n > 0) {
        return (int)n;
    }
    return socket_would_block_or_interrupted() ? -2 : -1;
}

static int buffer_empty(const buffer_t *buffer) {
    return buffer->off >= buffer->len;
}

static void buffer_reset(buffer_t *buffer) {
    buffer->off = 0;
    buffer->len = 0;
}

static int proxy_loop(stream_t *client, stream_t *upstream) {
    buffer_t c2u;
    buffer_t u2c;
    int client_open = 1;
    int upstream_open = 1;

    buffer_reset(&c2u);
    buffer_reset(&u2c);
    (void)set_nonblocking(client->fd);
    (void)set_nonblocking(upstream->fd);

    for (;;) {
        int progress = 0;

        if (!buffer_empty(&c2u)) {
            int n = stream_write(upstream, c2u.data + c2u.off, c2u.len - c2u.off);
            if (n > 0) {
                c2u.off += (size_t)n;
                if (buffer_empty(&c2u)) {
                    buffer_reset(&c2u);
                }
                progress = 1;
            } else if (n < -1) {
                /* retry later */
            } else {
                break;
            }
        }

        if (!buffer_empty(&u2c)) {
            int n = stream_write(client, u2c.data + u2c.off, u2c.len - u2c.off);
            if (n > 0) {
                u2c.off += (size_t)n;
                if (buffer_empty(&u2c)) {
                    buffer_reset(&u2c);
                }
                progress = 1;
            } else if (n < -1) {
                /* retry later */
            } else {
                break;
            }
        }

        if (client_open && buffer_empty(&c2u)) {
            int n = stream_read(client, c2u.data, sizeof(c2u.data));
            if (n > 0) {
                c2u.off = 0;
                c2u.len = (size_t)n;
                progress = 1;
            } else if (n == 0) {
                client_open = 0;
                break;
            } else if (n < -1) {
                /* retry later */
            } else {
                break;
            }
        }

        if (upstream_open && buffer_empty(&u2c)) {
            int n = stream_read(upstream, u2c.data, sizeof(u2c.data));
            if (n > 0) {
                u2c.off = 0;
                u2c.len = (size_t)n;
                progress = 1;
            } else if (n == 0) {
                upstream_open = 0;
                break;
            } else if (n < -1) {
                /* retry later */
            } else {
                break;
            }
        }

        if (!progress) {
            fd_set readfds;
            fd_set writefds;
            int maxfd = client->fd > upstream->fd ? client->fd : upstream->fd;
            struct timeval tv;

            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            if (client_open && buffer_empty(&c2u)) {
                FD_SET(client->fd, &readfds);
            }
            if (upstream_open && buffer_empty(&u2c)) {
                FD_SET(upstream->fd, &readfds);
            }
            if (!buffer_empty(&c2u)) {
                FD_SET(upstream->fd, &writefds);
            }
            if (!buffer_empty(&u2c)) {
                FD_SET(client->fd, &writefds);
            }

            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(maxfd + 1, &readfds, &writefds, NULL, &tv) < 0 && errno != EINTR) {
                break;
            }
        }
    }

    return 0;
}

static void *client_thread(void *opaque) {
    client_arg_t *arg = (client_arg_t *)opaque;
    proxy_config_t *config = arg->config;
    stream_t client;
    stream_t upstream;
    char addr_text[128];
    void *addr_ptr = NULL;
    int addr_port = 0;

    memset(&client, 0, sizeof(client));
    memset(&upstream, 0, sizeof(upstream));
    client.fd = -1;
    upstream.fd = -1;

    if (arg->addr.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&arg->addr;
        addr_ptr = &sin->sin_addr;
        addr_port = ntohs(sin->sin_port);
    } else if (arg->addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&arg->addr;
        addr_ptr = &sin6->sin6_addr;
        addr_port = ntohs(sin6->sin6_port);
    }
    if (addr_ptr != NULL) {
        inet_ntop(arg->addr.ss_family, addr_ptr, addr_text, sizeof(addr_text));
    } else {
        snprintf(addr_text, sizeof(addr_text), "?");
    }

    printf("[PROXY] client %s:%d connected\n", addr_text, addr_port);
    if (stream_open_client(&client, arg->fd, config) != 0) {
        free(arg);
        return NULL;
    }
    arg->fd = -1;
    printf("[PROXY] client %s:%d protocol=%s\n",
           addr_text, addr_port, client.use_tls ? "tls" : "tcp");

    if (stream_open_upstream(&upstream, &config->upstream, config->upstream_verify) != 0) {
        fprintf(stderr, "[PROXY] upstream connect failed\n");
        stream_close(&client);
        free(arg);
        return NULL;
    }

    printf("[PROXY] forwarding %s:%d -> %s://%s:%s\n",
           addr_text,
           addr_port,
           config->upstream.use_tls ? "tls" : "tcp",
           config->upstream.host,
           config->upstream.port);
    (void)proxy_loop(&client, &upstream);
    stream_close(&upstream);
    stream_close(&client);
    printf("[PROXY] client %s:%d closed\n", addr_text, addr_port);
    free(arg);
    return NULL;
}

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s\n", argv0);
    printf("  %s --config proxy.json\n", argv0);
    printf("  %s --listen-auto host:port --upstream stratum+tls://public-pool.io:4333\n", argv0);
    printf("  %s --listen-tcp host:port --upstream stratum+tls://public-pool.io:4333\n", argv0);
    printf("  %s --listen-tls host:port --cert fullchain.pem --key privkey.pem --upstream stratum+tls://public-pool.io:4333\n", argv0);
    printf("\nOptions:\n");
    printf("  --config path              Load proxy settings from JSON. Default: proxy.json if present.\n");
    printf("  --listen-auto host:port    Auto-detect plain TCP or TLS on the same port. Default.\n");
    printf("  --listen-tcp host:port     Listen without TLS, useful for local tests.\n");
    printf("  --listen-tls host:port     Require TLS for every client.\n");
    printf("  --data-dir path            Persistent directory for auto-generated certificate and key.\n");
    printf("  --cert path --key path     Use an existing TLS certificate and private key.\n");
    printf("  --upstream url             Upstream stratum+tls:// or stratum+tcp:// URL.\n");
    printf("  --upstream-no-verify       Disable upstream TLS certificate verification.\n");
}

static int file_exists(const char *path) {
    return path != NULL && path[0] != '\0' && BTC_ACCESS(path, BTC_R_OK) == 0;
}

static void print_certificate_fingerprint(const char *cert_path) {
    FILE *fp = fopen(cert_path, "r");
    X509 *cert = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (fp == NULL) {
        return;
    }
    cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (cert == NULL || X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1) {
        X509_free(cert);
        return;
    }

    printf("[PROXY] certificate=%s sha256=", cert_path);
    for (unsigned int i = 0; i < digest_len; ++i) {
        printf("%s%02X", i == 0 ? "" : ":", digest[i]);
    }
    printf("\n");
    X509_free(cert);
}

static int generate_self_signed_cert(const char *cert_path, const char *key_path) {
    EVP_PKEY_CTX *pctx = NULL;
    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    FILE *fp = NULL;
    int ok = 0;

    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (pctx == NULL ||
        EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        goto done;
    }

    cert = X509_new();
    if (cert == NULL) {
        goto done;
    }

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)(time(NULL) & 0x7fffffff));
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60L * 60L * 24L * 365L * 10L);
    X509_set_pubkey(cert, pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char *)"CN", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"BTCRig", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"btc-proxy.local", -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, EVP_sha256()) <= 0) {
        goto done;
    }

    fp = fopen(key_path, "w");
    if (fp == NULL || PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        goto done;
    }
    fclose(fp);
    fp = NULL;
#if !defined(_WIN32)
    (void)chmod(key_path, S_IRUSR | S_IWUSR);
#endif

    fp = fopen(cert_path, "w");
    if (fp == NULL || PEM_write_X509(fp, cert) != 1) {
        goto done;
    }
    fclose(fp);
    fp = NULL;
    ok = 1;

done:
    if (fp != NULL) {
        fclose(fp);
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    if (!ok) {
        fprintf(stderr, "[PROXY] failed to generate self-signed certificate\n");
    }
    return ok ? 0 : -1;
}

static const char *find_config_arg(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int has_help_arg(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *listen_mode_name(int mode) {
    if (mode == LISTEN_TLS) {
        return "tls";
    }
    if (mode == LISTEN_TCP) {
        return "tcp";
    }
    return "auto";
}

static int json_bool_value(json_t *value, int fallback) {
    if (json_is_boolean(value)) {
        return json_is_true(value) ? 1 : 0;
    }
    return fallback;
}

static void json_string_copy(json_t *value, char *dst, size_t dst_size) {
    const char *text = json_string_value(value);
    if (text != NULL) {
        copy_text(dst, dst_size, text);
    }
}

static void json_port_copy(json_t *value, char *dst, size_t dst_size) {
    if (json_is_string(value)) {
        json_string_copy(value, dst, dst_size);
    } else if (json_is_integer(value)) {
        snprintf(dst, dst_size, "%lld", (long long)json_integer_value(value));
    }
}

static int set_listen_url(proxy_config_t *config, const char *url) {
    endpoint_t endpoint;
    if (parse_endpoint(url, &endpoint) != 0) {
        return -1;
    }
    copy_text(config->listen_host, sizeof(config->listen_host), endpoint.host);
    copy_text(config->listen_port, sizeof(config->listen_port), endpoint.port);
    config->listen_mode = endpoint.use_tls ? LISTEN_TLS : LISTEN_TCP;
    return 0;
}

static int load_proxy_config(proxy_config_t *config, const char *path, int required) {
    json_error_t error;
    json_t *root = json_load_file(path, 0, &error);
    if (root == NULL) {
        if (required) {
            fprintf(stderr, "[PROXY] failed to load %s line=%d col=%d: %s\n",
                    path, error.line, error.column, error.text);
            return -1;
        }
        return 0;
    }

    printf("[PROXY] loaded %s\n", path);

    json_t *listen = json_object_get(root, "listen");
    if (json_is_string(listen)) {
        if (set_listen_url(config, json_string_value(listen)) != 0) {
            fprintf(stderr, "[PROXY] invalid listen URL in %s\n", path);
            json_decref(root);
            return -1;
        }
    } else if (json_is_object(listen)) {
        json_t *url = json_object_get(listen, "url");
        if (json_is_string(url) && set_listen_url(config, json_string_value(url)) != 0) {
            fprintf(stderr, "[PROXY] invalid listen.url in %s\n", path);
            json_decref(root);
            return -1;
        }

        json_t *mode = json_object_get(listen, "mode");
        const char *mode_text = json_string_value(mode);
        if (mode_text != NULL) {
            if (strcmp(mode_text, "tls") == 0 || strcmp(mode_text, "ssl") == 0) {
                config->listen_mode = LISTEN_TLS;
            } else if (strcmp(mode_text, "tcp") == 0) {
                config->listen_mode = LISTEN_TCP;
            } else if (strcmp(mode_text, "auto") == 0) {
                config->listen_mode = LISTEN_AUTO;
            }
        }
        json_t *tls = json_object_get(listen, "tls");
        if (json_is_boolean(tls)) {
            config->listen_mode = json_is_true(tls) ? LISTEN_TLS : LISTEN_AUTO;
        }
        json_string_copy(json_object_get(listen, "host"), config->listen_host, sizeof(config->listen_host));
        json_port_copy(json_object_get(listen, "port"), config->listen_port, sizeof(config->listen_port));
        json_string_copy(json_object_get(listen, "cert"), config->cert_path, sizeof(config->cert_path));
        json_string_copy(json_object_get(listen, "key"), config->key_path, sizeof(config->key_path));
    }

    json_t *upstream = json_object_get(root, "upstream");
    if (json_is_string(upstream)) {
        if (parse_endpoint(json_string_value(upstream), &config->upstream) != 0) {
            fprintf(stderr, "[PROXY] invalid upstream URL in %s\n", path);
            json_decref(root);
            return -1;
        }
    } else if (json_is_object(upstream)) {
        json_t *url = json_object_get(upstream, "url");
        if (json_is_string(url) && parse_endpoint(json_string_value(url), &config->upstream) != 0) {
            fprintf(stderr, "[PROXY] invalid upstream.url in %s\n", path);
            json_decref(root);
            return -1;
        }
        config->upstream_verify = json_bool_value(json_object_get(upstream, "verify"), config->upstream_verify);
    }

    json_string_copy(json_object_get(root, "cert"), config->cert_path, sizeof(config->cert_path));
    json_string_copy(json_object_get(root, "key"), config->key_path, sizeof(config->key_path));
    json_string_copy(json_object_get(root, "data-dir"), config->data_dir, sizeof(config->data_dir));
    config->upstream_verify = json_bool_value(json_object_get(root, "upstream-verify"), config->upstream_verify);

    json_decref(root);
    return 0;
}

int main(int argc, char **argv) {
    proxy_config_t config;
    const char *config_path = NULL;
    int config_required = 0;
    int listen_fd = -1;

    configure_stdio();
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    SSL_library_init();
    SSL_load_error_strings();

    memset(&config, 0, sizeof(config));
    copy_text(config.listen_host, sizeof(config.listen_host), "0.0.0.0");
    copy_text(config.listen_port, sizeof(config.listen_port), "4333");
    config.listen_mode = LISTEN_AUTO;
    config.upstream_verify = 0;
    set_default_data_dir(config.data_dir, sizeof(config.data_dir));
    if (parse_endpoint("stratum+tls://public-pool.io:4333", &config.upstream) != 0) {
        return 1;
    }

    if (has_help_arg(argc, argv)) {
        usage(argv[0]);
        return 0;
    }

    config_path = find_config_arg(argc, argv);
    if (config_path != NULL) {
        config_required = 1;
    } else if (BTC_ACCESS("proxy.json", BTC_R_OK) == 0) {
        config_path = "proxy.json";
    } else if (BTC_ACCESS("../proxy.json", BTC_R_OK) == 0) {
        config_path = "../proxy.json";
    }
    if (config_path != NULL && load_proxy_config(&config, config_path, config_required) != 0) {
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            ++i;
        } else if (strcmp(argv[i], "--listen-auto") == 0 && i + 1 < argc) {
            if (parse_host_port(argv[++i], config.listen_host, sizeof(config.listen_host),
                                config.listen_port, sizeof(config.listen_port)) != 0) {
                fprintf(stderr, "[PROXY] invalid --listen-auto\n");
                return 2;
            }
            config.listen_mode = LISTEN_AUTO;
        } else if (strcmp(argv[i], "--listen-tcp") == 0 && i + 1 < argc) {
            if (parse_host_port(argv[++i], config.listen_host, sizeof(config.listen_host),
                                config.listen_port, sizeof(config.listen_port)) != 0) {
                fprintf(stderr, "[PROXY] invalid --listen-tcp\n");
                return 2;
            }
            config.listen_mode = LISTEN_TCP;
        } else if (strcmp(argv[i], "--listen-tls") == 0 && i + 1 < argc) {
            if (parse_host_port(argv[++i], config.listen_host, sizeof(config.listen_host),
                                config.listen_port, sizeof(config.listen_port)) != 0) {
                fprintf(stderr, "[PROXY] invalid --listen-tls\n");
                return 2;
            }
            config.listen_mode = LISTEN_TLS;
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            copy_text(config.data_dir, sizeof(config.data_dir), argv[++i]);
        } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            copy_text(config.cert_path, sizeof(config.cert_path), argv[++i]);
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            copy_text(config.key_path, sizeof(config.key_path), argv[++i]);
        } else if (strcmp(argv[i], "--upstream") == 0 && i + 1 < argc) {
            if (parse_endpoint(argv[++i], &config.upstream) != 0) {
                fprintf(stderr, "[PROXY] invalid --upstream\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--upstream-no-verify") == 0) {
            config.upstream_verify = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (config.listen_mode != LISTEN_TCP) {
        if (config.cert_path[0] == '\0' && config.key_path[0] == '\0') {
            if (ensure_directory(config.data_dir) != 0) {
                fprintf(stderr, "[PROXY] failed to create data directory: %s\n", config.data_dir);
                return 1;
            }
            join_path(config.cert_path, sizeof(config.cert_path), config.data_dir, "cert.pem");
            join_path(config.key_path, sizeof(config.key_path), config.data_dir, "cert_key.pem");
        } else if (config.cert_path[0] == '\0' || config.key_path[0] == '\0') {
            fprintf(stderr, "[PROXY] TLS listen requires both cert and key paths\n");
            return 2;
        }

        if (!file_exists(config.cert_path) || !file_exists(config.key_path)) {
            printf("[PROXY] generating self-signed certificate cert=%s key=%s\n",
                   config.cert_path, config.key_path);
            if (generate_self_signed_cert(config.cert_path, config.key_path) != 0) {
                return 1;
            }
        }

        config.server_ctx = SSL_CTX_new(TLS_server_method());
        if (config.server_ctx == NULL) {
            fprintf(stderr, "[PROXY] SSL_CTX_new server failed\n");
            return 1;
        }
        configure_compat_tls_server(config.server_ctx);
        if (SSL_CTX_use_certificate_chain_file(config.server_ctx, config.cert_path) != 1 ||
            SSL_CTX_use_PrivateKey_file(config.server_ctx, config.key_path, SSL_FILETYPE_PEM) != 1) {
            fprintf(stderr, "[PROXY] failed to load certificate/key\n");
            SSL_CTX_free(config.server_ctx);
            return 1;
        }
        print_certificate_fingerprint(config.cert_path);
    }

    listen_fd = create_listener(config.listen_host, config.listen_port);
    if (listen_fd < 0) {
        perror("[PROXY] listen");
        if (config.server_ctx != NULL) {
            SSL_CTX_free(config.server_ctx);
        }
        return 1;
    }

    printf("[PROXY] listen %s://%s:%s -> %s://%s:%s upstream_verify=%s data_dir=%s\n",
           listen_mode_name(config.listen_mode),
           config.listen_host,
           config.listen_port,
           config.upstream.use_tls ? "tls" : "tcp",
           config.upstream.host,
           config.upstream.port,
           config.upstream_verify ? "on" : "off",
           config.data_dir);

    for (;;) {
        client_arg_t *arg = calloc(1, sizeof(*arg));
        pthread_t thread;
        if (arg == NULL) {
            break;
        }
        arg->config = &config;
        arg->addr_len = sizeof(arg->addr);
        arg->fd = accept(listen_fd, (struct sockaddr *)&arg->addr, &arg->addr_len);
        if (arg->fd < 0) {
            free(arg);
            if (socket_would_block_or_interrupted()) {
                continue;
            }
            perror("[PROXY] accept");
            break;
        }

        if (pthread_create(&thread, NULL, client_thread, arg) != 0) {
            socket_close_fd(arg->fd);
            free(arg);
            continue;
        }
        pthread_detach(thread);
    }

    socket_close_fd(listen_fd);
    if (config.server_ctx != NULL) {
        SSL_CTX_free(config.server_ctx);
    }
    return 0;
}
