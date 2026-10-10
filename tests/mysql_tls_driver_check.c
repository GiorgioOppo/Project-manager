/* Isolated regression harness: loopback only, dummy credentials, no database. */
#include "db.h"
#include <mysql.h>

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct server { int listener; int received; pthread_t thread; };

static void *plaintext_mysql(void *argument) {
    struct server *server = argument;
    int peer = accept(server->listener, NULL, NULL);
    assert(peer >= 0);
    struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    unsigned char greeting[160] = {0}, packet[164] = {0};
    size_t used = 0;
    greeting[used++] = 10;
    memcpy(greeting + used, "8.4.0", 6); used += 6;
    greeting[used++] = 1; used += 3;
    memcpy(greeting + used, "12345678", 8); used += 8;
    used++;
    /* Valid protocol 4.1/native-password greeting, deliberately without SSL. */
    unsigned int flags = 1U | 8U | 512U | 32768U | (1U << 19);
    greeting[used++] = flags & 255U; greeting[used++] = flags >> 8;
    greeting[used++] = 45;
    greeting[used++] = 2; used++;
    greeting[used++] = flags >> 16; greeting[used++] = flags >> 24;
    greeting[used++] = 21; used += 10;
    memcpy(greeting + used, "abcdefghijkl", 13); used += 13;
    memcpy(greeting + used, "mysql_native_password", 22); used += 22;
    packet[0] = used & 255U; packet[1] = (used >> 8) & 255U;
    memcpy(packet + 4, greeting, used);
    assert(send(peer, packet, used + 4, 0) == (ssize_t)(used + 4));
    unsigned char response[1024];
    server->received = (int)recv(peer, response, sizeof(response), 0);
    close(peer);
    close(server->listener);
    return NULL;
}

static unsigned int start_server(struct server *server) {
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    server->received = -999;
    assert(server->listener >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    assert(bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(server->listener, (struct sockaddr *)&address, &size) == 0);
    assert(listen(server->listener, 1) == 0);
    assert(pthread_create(&server->thread, NULL, plaintext_mysql, server) == 0);
    return ntohs(address.sin_port);
}

static void expect_configuration_failure(const char *mode, const char *ca,
                                          const char *diagnostic) {
    if (mode) assert(setenv("DB_TLS_MODE", mode, 1) == 0);
    else assert(unsetenv("DB_TLS_MODE") == 0);
    if (ca) assert(setenv("DB_SSL_CA", ca, 1) == 0);
    else assert(unsetenv("DB_SSL_CA") == 0);
    /* Check the specific configuration error, so a network failure cannot
       accidentally make a missing validation look like a successful test. */
    fflush(stderr);
    FILE *capture = tmpfile();
    int saved = dup(STDERR_FILENO);
    assert(capture && saved >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    Db *db = (Db *)1;
    int result = db_open_v2(NULL, &db, DB_OPEN_READWRITE, NULL);
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(capture);
    char message[512] = {0};
    assert(fread(message, 1, sizeof(message) - 1, capture) > 0);
    fclose(capture);
    assert(result == DB_ERROR && db == NULL);
    assert(strstr(message, diagnostic));
}

int main(void) {
    assert(setenv("DB_PASSWORD", "dummy-in-isolated-test", 1) == 0);
    assert(setenv("DB_USER", "tls_test_user", 1) == 0);
    assert(setenv("DB_NAME", "tls_test_database", 1) == 0);
    assert(setenv("DB_HOST", "127.0.0.1", 1) == 0);
    assert(setenv("DB_PORT", "9", 1) == 0);
    char directory[] = "/tmp/punto-tls-check-XXXXXX";
    assert(mkdtemp(directory));
    char empty_path[128], missing_path[128], dummy_path[128];
    assert(snprintf(empty_path, sizeof(empty_path), "%s/empty.pem", directory) > 0);
    assert(snprintf(missing_path, sizeof(missing_path), "%s/missing.pem", directory) > 0);
    assert(snprintf(dummy_path, sizeof(dummy_path), "%s/dummy.pem", directory) > 0);
    FILE *empty = fopen(empty_path, "wb");
    assert(empty); fclose(empty);
    expect_configuration_failure("disabled", NULL, "DB_TLS_MODE");
    expect_configuration_failure("required", NULL, "DB_TLS_MODE");
    expect_configuration_failure("invalid", NULL, "DB_TLS_MODE");
    expect_configuration_failure(NULL, NULL, "DB_SSL_CA");
    expect_configuration_failure("verify_identity", "relative.pem", "file assoluto leggibile");
    expect_configuration_failure("verify_identity", missing_path, "file assoluto leggibile");
    expect_configuration_failure("verify_identity", empty_path, "file assoluto leggibile");

    /* Control: the fixture really accepts a normal native-password handshake.
       This client deliberately sends dummy auth material to our own loopback
       listener. The application driver below must send zero bytes instead. */
    struct server control;
    unsigned int port = start_server(&control);
    MYSQL *native = mysql_init(NULL);
    assert(native);
    my_bool off = 0;
    unsigned int timeout = 3;
    assert(mysql_options(native, MYSQL_OPT_SSL_ENFORCE, &off) == 0);
    assert(mysql_options(native, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off) == 0);
    assert(mysql_options(native, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) == 0);
    assert(mysql_options(native, MYSQL_OPT_READ_TIMEOUT, &timeout) == 0);
    (void)mysql_real_connect(native, "127.0.0.1", "tls_test_user", "dummy",
                             "tls_test_database", port, NULL, 0);
    mysql_close(native);
    assert(pthread_join(control.thread, NULL) == 0);
    assert(control.received > 36);

    struct server protected;
    port = start_server(&protected);
    char port_text[12];
    assert(snprintf(port_text, sizeof(port_text), "%u", port) > 0);
    assert(setenv("DB_PORT", port_text, 1) == 0);
    /* A readable nonempty CA placeholder lets this probe reach the greeting.
       Lack of SSL must reject it before certificates/auth are exchanged. */
    FILE *dummy = fopen(dummy_path, "wb");
    assert(dummy); assert(fputs("test", dummy) >= 0); fclose(dummy);
    assert(unsetenv("DB_TLS_MODE") == 0);
    assert(setenv("DB_SSL_CA", dummy_path, 1) == 0);
    Db *db = NULL;
    assert(db_open_v2(NULL, &db, DB_OPEN_READWRITE, NULL) == DB_ERROR);
    assert(db == NULL);
    assert(pthread_join(protected.thread, NULL) == 0);
    assert(protected.received == 0);
    assert(unlink(empty_path) == 0 && unlink(dummy_path) == 0 && rmdir(directory) == 0);
    puts("8 TLS rejection cases passed; no auth bytes sent to the plaintext server.");
    return 0;
}
