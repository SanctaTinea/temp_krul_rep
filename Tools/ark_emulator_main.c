#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_handle_t;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_handle_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCKET close
#endif

#include "ark_emulator_server.h"

#define RESPONSE_CAPACITY (16U * 1024U)
#define DEFAULT_PORT 7000U

static ark_emulator_server_t* emulator;

static bool send_all(socket_handle_t socket, const uint8_t* data, size_t size) {
    while (size > 0U) {
        int chunk = send(socket, (const char*)data,
                         size > 16384U ? 16384 : (int)size, 0);
        if (chunk == SOCKET_ERROR || chunk == 0) return false;
        data += (size_t)chunk;
        size -= (size_t)chunk;
    }
    return true;
}

static size_t dispatch_frame(krul_transport_format_t format,
                             const uint8_t* payload, size_t payload_size,
                             uint8_t* response, size_t capacity) {
    krul_server_t* server =
        ark_emulator_server_krul_for_format(emulator, format);
    if (server == NULL ||
        krul_dispatch(server, payload, payload_size) !=
            KRUL_DISPATCH_RESPONSE_READY)
        return 0U;
    size_t size = krul_transport_encode(
        format, krul_response_get_data(server), krul_response_get_size(server),
        response, capacity);
    krul_response_release(server);
    return size;
}

static bool serve_socket_client(socket_handle_t client) {
    uint8_t payload[ARK_EMULATOR_FRAME_SIZE];
    uint8_t input[1024];
    uint8_t response[RESPONSE_CAPACITY];
    krul_transport_parser_t parser;
    if (!krul_transport_parser_init(&parser, payload, sizeof(payload)))
        return false;
    for (;;) {
        int received = recv(client, (char*)input, sizeof(input), 0);
        if (received <= 0) return true;
        size_t offset = 0U;
        while (offset < (size_t)received) {
            size_t consumed = 0U;
            krul_transport_status_t status = krul_transport_parser_consume(
                &parser, input + offset, (size_t)received - offset, &consumed);
            offset += consumed;
            if (status == KRUL_TRANSPORT_FRAME_READY) {
                size_t response_size = dispatch_frame(
                    parser.format, parser.payload, parser.payload_size,
                    response, sizeof(response));
                if (response_size == 0U ||
                    !send_all(client, response, response_size))
                    return false;
                krul_transport_parser_reset(&parser);
            } else if (status == KRUL_TRANSPORT_INVALID_STATE) {
                return false;
            } else if (status == KRUL_TRANSPORT_NEED_MORE) {
                break;
            }
        }
    }
}

static int serve_stdio(void) {
    uint8_t payload[ARK_EMULATOR_FRAME_SIZE];
    uint8_t input[1024];
    uint8_t response[RESPONSE_CAPACITY];
    krul_transport_parser_t parser;
    if (!krul_transport_parser_init(&parser, payload, sizeof(payload))) return 1;
    for (;;) {
        size_t received = fread(input, 1U, sizeof(input), stdin);
        if (received == 0U) return ferror(stdin) ? 1 : 0;
        size_t offset = 0U;
        while (offset < received) {
            size_t consumed = 0U;
            krul_transport_status_t status = krul_transport_parser_consume(
                &parser, input + offset, received - offset, &consumed);
            offset += consumed;
            if (status == KRUL_TRANSPORT_FRAME_READY) {
                size_t response_size = dispatch_frame(
                    parser.format, parser.payload, parser.payload_size,
                    response, sizeof(response));
                if (response_size == 0U ||
                    fwrite(response, 1U, response_size, stdout) !=
                        response_size ||
                    fflush(stdout) != 0)
                    return 1;
                krul_transport_parser_reset(&parser);
            } else if (status == KRUL_TRANSPORT_INVALID_STATE) {
                return 1;
            } else if (status == KRUL_TRANSPORT_NEED_MORE) {
                break;
            }
        }
    }
}

static int serve_tcp(uint16_t port) {
#ifdef _WIN32
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
#endif
    socket_handle_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 1;
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     (const char*)&reuse, (int)sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (const struct sockaddr*)&address, sizeof(address)) ==
            SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        fprintf(stderr, "Cannot listen on 127.0.0.1:%u\n", (unsigned)port);
        CLOSE_SOCKET(listener);
        return 1;
    }
    fprintf(stderr, "ARK emulator listening on 127.0.0.1:%u\n",
            (unsigned)port);
    for (;;) {
        socket_handle_t client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        (void)serve_socket_client(client);
        CLOSE_SOCKET(client);
    }
    CLOSE_SOCKET(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

static bool parse_port(const char* text, uint16_t* port) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1L || value > 65535L)
        return false;
    *port = (uint16_t)value;
    return true;
}

int main(int argc, char** argv) {
    bool stdio_mode = false;
    uint16_t port = DEFAULT_PORT;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--stdio") == 0) stdio_mode = true;
        else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_port(argv[++index], &port)) return 2;
        } else {
            fprintf(stderr, "Usage: %s [--stdio] [--port PORT]\n", argv[0]);
            return 2;
        }
    }
    emulator = ark_emulator_server_create();
    if (emulator == NULL) {
        fputs("Cannot initialize ARK emulator\n", stderr);
        return 1;
    }
    int status = stdio_mode ? serve_stdio() : serve_tcp(port);
    ark_emulator_server_destroy(emulator);
    return status;
}
