#include "chat.h"
#include "chat_client.h"
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

struct chat_client {
    int socket;
    char *out_buf;
    size_t out_buf_size;
    size_t out_buf_pos; // Позиция отправленных данных
    char *in_buf;
    size_t in_buf_size;
    size_t in_buf_pos;
    struct chat_message *messages_head;
    struct chat_message *messages_tail;
    bool connecting;
};

struct chat_client *
chat_client_new(const char *name)
{
    (void)name; // Игнорируем имя для базовой версии
    struct chat_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        abort();
    }
    client->socket = -1;
    client->connecting = false;
    client->out_buf_pos = 0;
    return client;
}

void
chat_client_delete(struct chat_client *client)
{
    if (client == NULL) {
        return;
    }
    if (client->socket >= 0) {
        close(client->socket);
    }
    free(client->out_buf);
    free(client->in_buf);
    struct chat_message *msg = client->messages_head;
    while (msg) {
        struct chat_message *next = msg->next;
        chat_message_delete(msg);
        msg = next;
    }
    free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
    if (client == NULL || addr == NULL) {
        fprintf(stderr, "[ERROR] Invalid arguments\n");
        return CHAT_ERR_INVALID_ARGUMENT;
    }
    if (client->socket >= 0) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    char host[256], port[16];
    if (sscanf(addr, "%255[^:]:%15s", host, port) != 2) {
        return CHAT_ERR_NO_ADDR;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        return CHAT_ERR_NO_ADDR;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return CHAT_ERR_SYS;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        freeaddrinfo(res);
        return CHAT_ERR_SYS;
    }

    rc = connect(sock, res->ai_addr, res->ai_addrlen);
    if (rc < 0) {
        if (errno == EINPROGRESS) {
            client->connecting = true;
        } else {
            close(sock);
            freeaddrinfo(res);
            return CHAT_ERR_SYS;
        }
    } else {
        client->connecting = false;
    }

    client->socket = sock;
    freeaddrinfo(res);

    // Wait for connection to complete if in progress
    if (client->connecting) {
        struct pollfd pfd = { .fd = client->socket, .events = POLLOUT };
        while (client->connecting) {
            int poll_result = poll(&pfd, 1, 100); // 100 ms timeout
            if (poll_result > 0 && (pfd.revents & POLLOUT)) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(client->socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    client->connecting = false;
                } else {
                    close(client->socket);
                    client->socket = -1;
                    return CHAT_ERR_SYS;
                }
            } else if (poll_result < 0) {
                close(client->socket);
                client->socket = -1;
                return CHAT_ERR_SYS;
            }
            // If poll_result == 0, timeout occurred, loop again
        }
    }

    return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
    if (client == NULL || client->messages_head == NULL) {
        return NULL;
    }

    struct chat_message *msg = client->messages_head;
    client->messages_head = msg->next;
    if (client->messages_head == NULL) {
        client->messages_tail = NULL;
    }
    msg->next = NULL;
    return msg;
}

int chat_client_update(struct chat_client *client, double timeout) {
    if (client->socket < 0) return CHAT_ERR_NOT_STARTED;

    struct pollfd pfd = { .fd = client->socket };
    if (client->connecting) pfd.events = POLLOUT;
    else pfd.events = POLLIN | (client->out_buf_size > client->out_buf_pos ? POLLOUT : 0);

    int poll_result = poll(&pfd, 1, (int)(timeout * 1000));
    if (poll_result < 0) return CHAT_ERR_SYS;
    if (poll_result == 0) return CHAT_ERR_TIMEOUT;

    if (client->connecting && (pfd.revents & POLLOUT)) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(client->socket, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            close(client->socket);
            client->socket = -1;
            return CHAT_ERR_SYS;
        }
        client->connecting = false;
    }

    // Handle POLLOUT for sending data
    if (!client->connecting && (pfd.revents & POLLOUT)) {
        if (client->out_buf_pos < client->out_buf_size) {
            ssize_t sent = send(client->socket, client->out_buf + client->out_buf_pos,
                                client->out_buf_size - client->out_buf_pos, 0);
            if (sent > 0) client->out_buf_pos += sent;
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return CHAT_ERR_SYS;
        }
    }

    // Handle POLLIN for reading data
    if (!client->connecting && (pfd.revents & POLLIN)) {
        // Existing code for reading data can be added here
    }

    return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
    return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
    if (client->socket < 0) {
        return 0;
    }
    if (client->connecting) {
        return CHAT_EVENT_OUTPUT;
    }
    int events = CHAT_EVENT_INPUT;
    if (client->out_buf_pos < client->out_buf_size) {
        events |= CHAT_EVENT_OUTPUT;
    }
    return events;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
    if (client == NULL || msg == NULL || client->socket < 0) {
        return CHAT_ERR_NOT_STARTED;
    }

    const char *start = msg;
    const char *end = memchr(msg, '\n', msg_size);
    if (end == NULL) {
        end = msg + msg_size;
    }

    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    if (start >= end) {
        return 0;
    }

    size_t new_msg_size = end - start;
    size_t total_size = client->out_buf_size + new_msg_size + 1;

    if (client->out_buf == NULL) {
        client->out_buf = malloc(total_size);
        if (client->out_buf == NULL) {
            return CHAT_ERR_SYS;
        }
        client->out_buf_size = 0;
        client->out_buf_pos = 0;
    } else {
        char *new_buf = realloc(client->out_buf, total_size);
        if (new_buf == NULL) {
            return CHAT_ERR_SYS;
        }
        client->out_buf = new_buf;
    }

    memcpy(client->out_buf + client->out_buf_size, start, new_msg_size);
    client->out_buf[client->out_buf_size + new_msg_size] = '\n';
    client->out_buf_size += new_msg_size + 1;

    return 0;
}