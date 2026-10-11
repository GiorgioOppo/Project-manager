#ifndef PUNTO_HTTP_TRANSPORT_H
#define PUNTO_HTTP_TRANSPORT_H

/* Single-threaded, bounded HTTP transport. Application handlers run only after
 * the complete request has arrived; slow sockets never own the application loop.
 * This header is private to one translation unit in each independent service. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HTTP_TRANSPORT_CLIENTS 64
#define HTTP_TRANSPORT_PEER_CLIENTS 8
#define HTTP_TRANSPORT_OUTPUT_LIMIT (16U * 1024U * 1024U)
#define HTTP_TRANSPORT_MEMORY_LIMIT (64U * 1024U * 1024U)

typedef void (*HttpTransportHandler)(int, char *, size_t, void *);
typedef int (*HttpTransportHeaders)(const char *, size_t, size_t, void *);
typedef void (*HttpTransportTick)(void *);
typedef struct {
    int fd;
    char peer[INET6_ADDRSTRLEN];
    char *input, *output;
    size_t used, input_capacity, header_size, total, output_size, output_capacity, sent;
    uint64_t accepted, deadline;
    bool writing, failed;
} HttpTransportClient;

static HttpTransportClient *http_transport_active;
static HttpTransportClient *http_transport_clients;
static size_t http_transport_memory;
static size_t http_transport_small_reserve, http_transport_header_limit;

static uint64_t http_transport_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void http_transport_erase(void *value, size_t size) {
    volatile unsigned char *p = value;
    while (size--) *p++ = 0;
}

static void http_transport_drop(HttpTransportClient *client);

static bool http_transport_resize(HttpTransportClient *owner, char **buffer,
                                  size_t *capacity, size_t needed) {
    if (needed <= *capacity) return true;
    size_t grown = *capacity ? *capacity * 2 : 4096;
    if (grown < needed) grown = needed;
    size_t extra = grown - *capacity;
    /* Large uploads/responses leave room for new small requests and errors. */
    size_t budget = HTTP_TRANSPORT_MEMORY_LIMIT;
    bool large = buffer == &owner->output ? grown > 4096 : grown > http_transport_header_limit + 1;
    if (large) budget -= http_transport_small_reserve;
    if (extra > budget) return false;
    while (http_transport_memory > budget || extra > budget - http_transport_memory) {
        HttpTransportClient *oldest = NULL;
        for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) {
            HttpTransportClient *candidate = &http_transport_clients[i];
            if (candidate == owner || candidate->fd < 0 || candidate->writing ||
                !(candidate->input_capacity + candidate->output_capacity)) continue;
            if (!oldest || candidate->accepted < oldest->accepted)
                oldest = candidate;
        }
        if (!oldest) return false;
        http_transport_drop(oldest);
    }
    char *replacement = malloc(grown);
    if (!replacement) return false;
    if (*buffer) {
        memcpy(replacement, *buffer, *capacity);
        http_transport_erase(*buffer, *capacity);
        free(*buffer);
    }
    http_transport_memory += grown - *capacity;
    *buffer = replacement;
    *capacity = grown;
    return true;
}

/* response()/respond() append to the active connection; poll drains it later. */
static bool http_transport_write(int fd, const void *data, size_t size) {
    HttpTransportClient *client = http_transport_active;
    if (!client || client->fd != fd || client->failed) return false;
    if (size > HTTP_TRANSPORT_OUTPUT_LIMIT - client->output_size ||
        !http_transport_resize(client, &client->output, &client->output_capacity,
                               client->output_size + size)) {
        client->failed = true;
        return false;
    }
    if (size) memcpy(client->output + client->output_size, data, size);
    client->output_size += size;
    return true;
}

static void http_transport_drop(HttpTransportClient *client) {
    if (client->fd >= 0) close(client->fd);
    if (client->input) {
        http_transport_erase(client->input, client->input_capacity);
        free(client->input);
    }
    if (client->output) {
        http_transport_erase(client->output, client->output_capacity);
        free(client->output);
    }
    http_transport_memory -= client->input_capacity + client->output_capacity;
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static void http_transport_error(HttpTransportClient *client, int status,
                                 unsigned timeout_seconds) {
    const char *reason = status == 408 ? "Request Timeout" :
        status == 413 ? "Payload Too Large" :
        status == 431 ? "Request Header Fields Too Large" :
        status == 503 ? "Service Unavailable" : "Bad Request";
    const char body[] = "{\"ok\":false,\"error\":\"Richiesta HTTP non valida.\"}";
    char header[768];
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n\r\n",
        status, reason, sizeof(body) - 1);
    http_transport_active = client;
    if (length <= 0 || (size_t)length >= sizeof(header) ||
        !http_transport_write(client->fd, header, (size_t)length) ||
        !http_transport_write(client->fd, body, sizeof(body) - 1)) client->failed = true;
    http_transport_active = NULL;
    client->writing = true;
    client->deadline = http_transport_now() + (uint64_t)timeout_seconds * 1000U;
}

/* Only framing is inspected here. The existing application parser remains the
 * authority for request lines, individual headers, hosts, and route body limits. */
static int http_transport_frame(HttpTransportClient *client, size_t header_limit,
                                size_t body_limit) {
    char *end = strstr(client->input, "\r\n\r\n");
    if (memchr(client->input, 0, end ? (size_t)(end - client->input) + 4 : client->used)) return 400;
    if (!end) return client->used >= header_limit ? 431 : 0;
    size_t header_size = (size_t)(end - client->input) + 4;
    if (header_size > header_limit) return 431;
    client->header_size = header_size;
    char *line = strstr(client->input, "\r\n");
    if (!line) return 400;
    line += 2;
    size_t content_length = 0;
    bool has_length = false;
    while (line < end) {
        char *next = strstr(line, "\r\n");
        char *colon = memchr(line, ':', next ? (size_t)(next - line) : 0);
        if (!next || next > end || !colon || colon == line) return 400;
        size_t name_length = (size_t)(colon - line);
        if (name_length == 17 && !strncasecmp(line, "Transfer-Encoding", 17)) return 400;
        if (name_length == 14 && !strncasecmp(line, "Content-Length", 14)) {
            if (has_length) return 400;
            has_length = true;
            char *value = colon + 1, *tail = next;
            while (value < tail && (*value == ' ' || *value == '\t')) value++;
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) tail--;
            if (value == tail) return 400;
            for (; value < tail; value++) {
                if (*value < '0' || *value > '9') return 400;
                size_t digit = (size_t)(*value - '0');
                if (content_length > body_limit / 10 ||
                    (content_length == body_limit / 10 && digit > body_limit % 10)) return 413;
                content_length = content_length * 10 + digit;
            }
        }
        line = next + 2;
    }
    client->total = header_size + content_length;
    return client->used > client->total ? 400 : 0;
}

static void http_transport_peer(char out[INET6_ADDRSTRLEN],
                                 const struct sockaddr_storage *address) {
    const void *value = NULL;
    if (address->ss_family == AF_INET)
        value = &((const struct sockaddr_in *)address)->sin_addr;
    else if (address->ss_family == AF_INET6)
        value = &((const struct sockaddr_in6 *)address)->sin6_addr;
    if (!value || !inet_ntop(address->ss_family, value, out, INET6_ADDRSTRLEN))
        strcpy(out, "unknown");
}

static bool http_transport_run(int listener, volatile sig_atomic_t *stopping,
                                size_t header_limit, size_t body_limit,
                                unsigned timeout_seconds,
                                HttpTransportHeaders check_headers,
                                HttpTransportHandler handler, HttpTransportTick tick,
                                void *context) {
    if (!header_limit || header_limit == SIZE_MAX ||
        body_limit > SIZE_MAX - header_limit - 1 || !timeout_seconds || !handler)
        return false;
    int flags = fcntl(listener, F_GETFL, 0);
    if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    HttpTransportClient clients[HTTP_TRANSPORT_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) clients[i].fd = -1;
    http_transport_clients = clients;
    http_transport_memory = 0;
    http_transport_header_limit = header_limit;
    http_transport_small_reserve = HTTP_TRANSPORT_CLIENTS *
        ((header_limit < 65536 ? header_limit : 65536) + 4096);
    bool ok = true;
    while (!*stopping) {
        if (tick) tick(context);
        struct pollfd descriptors[HTTP_TRANSPORT_CLIENTS + 1];
        descriptors[0] = (struct pollfd){listener, POLLIN, 0};
        uint64_t now = http_transport_now();
        for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) {
            HttpTransportClient *client = &clients[i];
            if (client->fd >= 0 && now >= client->deadline) {
                if (!client->writing) http_transport_error(client, 408, timeout_seconds);
                else http_transport_drop(client);
            }
            if (client->failed) http_transport_drop(client);
            descriptors[i + 1] = (struct pollfd){client->fd,
                client->writing ? POLLOUT : POLLIN, 0};
        }
        int count = poll(descriptors, HTTP_TRANSPORT_CLIENTS + 1, 100);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (!*stopping) ok = false;
            break;
        }
        if (*stopping) break;
        /* Process existing clients before admitting new ones: a newcomer cannot
         * evict a request which is already complete and ready in this poll. */
        for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) {
            HttpTransportClient *client = &clients[i];
            short events = descriptors[i + 1].revents;
            if (client->fd < 0 || !events) continue;
            if (events & (POLLERR | POLLNVAL)) { http_transport_drop(client); continue; }
            if (http_transport_now() >= client->deadline) {
                if (!client->writing) http_transport_error(client, 408, timeout_seconds);
                else http_transport_drop(client);
                continue;
            }
            if (client->writing) {
                if (events & POLLOUT) {
                    ssize_t sent = send(client->fd, client->output + client->sent,
                                        client->output_size - client->sent, 0);
                    if (sent > 0) client->sent += (size_t)sent;
                    else if (sent == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                        client->failed = true;
                }
                if (client->failed || client->sent == client->output_size || (events & POLLHUP))
                    http_transport_drop(client);
                continue;
            }
            size_t limit = client->total ? client->total : header_limit;
            size_t available = limit - client->used;
            if (available > 16384) available = 16384;
            if (!http_transport_resize(client, &client->input, &client->input_capacity,
                                       client->used + available + 1)) {
                http_transport_error(client, 503, timeout_seconds);
                continue;
            }
            ssize_t received = recv(client->fd, client->input + client->used, available, 0);
            if (received <= 0) {
                if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (client->used) http_transport_error(client, 400, timeout_seconds);
                else http_transport_drop(client);
                continue;
            }
            client->used += (size_t)received;
            client->input[client->used] = 0;
            bool had_headers = client->total != 0;
            int status = had_headers ? 0 : http_transport_frame(client, header_limit, body_limit);
            if (!status && !had_headers && client->total && check_headers)
                status = check_headers(client->input, client->header_size,
                                       client->total - client->header_size, context);
            if (status) { http_transport_error(client, status, timeout_seconds); continue; }
            if (client->total && client->used == client->total) {
                http_transport_active = client;
                handler(client->fd, client->input, client->used, context);
                http_transport_active = NULL;
                client->writing = true;
                client->deadline = http_transport_now() + (uint64_t)timeout_seconds * 1000U;
                http_transport_erase(client->input, client->input_capacity);
                http_transport_memory -= client->input_capacity;
                free(client->input);
                client->input = NULL;
                client->input_capacity = 0;
                if (client->failed || !client->output_size) http_transport_drop(client);
            }
        }
        if (descriptors[0].revents & POLLIN) {
            /* Accept a bounded batch so a connection flood cannot starve I/O. */
            for (size_t batch = 0; batch < HTTP_TRANSPORT_CLIENTS; batch++) {
                struct sockaddr_storage address;
                socklen_t address_size = sizeof(address);
                int fd = accept(listener, (struct sockaddr *)&address, &address_size);
                if (fd < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK && !*stopping) ok = false;
                    break;
                }
                int client_flags = fcntl(fd, F_GETFL, 0);
                if (client_flags < 0 || fcntl(fd, F_SETFL, client_flags | O_NONBLOCK) < 0) {
                    close(fd); continue;
                }
                char peer[INET6_ADDRSTRLEN];
                http_transport_peer(peer, &address);
                size_t peers = 0;
                HttpTransportClient *slot = NULL, *oldest = NULL, *oldest_peer = NULL;
                for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) {
                    HttpTransportClient *candidate = &clients[i];
                    if (candidate->fd < 0) { if (!slot) slot = candidate; continue; }
                    if (candidate->writing) continue;
                    if (!oldest || candidate->accepted < oldest->accepted)
                        oldest = candidate;
                    if (!strcmp(candidate->peer, peer)) {
                        peers++;
                        if (!oldest_peer || candidate->accepted < oldest_peer->accepted)
                            oldest_peer = candidate;
                    }
                }
                if (peers >= HTTP_TRANSPORT_PEER_CLIENTS) slot = oldest_peer;
                else if (!slot) slot = oldest;
                if (!slot) { close(fd); continue; }
                if (slot->fd >= 0) http_transport_drop(slot);
                slot->fd = fd;
                strcpy(slot->peer, peer);
                slot->accepted = http_transport_now();
                slot->deadline = slot->accepted + (uint64_t)timeout_seconds * 1000U;
            }
        }
    }
    for (size_t i = 0; i < HTTP_TRANSPORT_CLIENTS; i++) http_transport_drop(&clients[i]);
    http_transport_active = NULL;
    http_transport_clients = NULL;
    return ok;
}

#endif
