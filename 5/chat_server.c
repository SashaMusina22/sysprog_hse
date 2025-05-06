#include "chat.h"
#include "chat_server.h"

#include <sys/event.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define MAX_EVENTS 10
#define BUFFER_SIZE 65536  // Увеличен размер буфера до 64 КБ

struct chat_peer {
    int socket;
    char *out_buf;
    size_t out_buf_size;
    size_t out_buf_pos;
    char *in_buf; // Буфер для накопления входящих данных
    size_t in_buf_size;
    size_t in_buf_pos;
#if NEED_AUTHOR
    char *name;
#endif
};

struct chat_server {
    int socket;
    int kq;
    struct chat_peer **peers;
    size_t peer_count;
    size_t peer_capacity;
    struct chat_message *messages;
};

static struct chat_peer*
peer_new(int socket)
{
    struct chat_peer *peer = calloc(1, sizeof(*peer));
    if (!peer) abort();
    peer->socket = socket;
    peer->in_buf = NULL;
    peer->in_buf_size = 0;
    peer->in_buf_pos = 0;
    return peer;
}

static void
peer_delete(struct chat_peer *peer)
{
    if (peer->socket >= 0) close(peer->socket);
    free(peer->out_buf);
    free(peer->in_buf);
#if NEED_AUTHOR
    free(peer->name);
#endif
    free(peer);
}

static void
server_add_peer(struct chat_server *server, struct chat_peer *peer)
{
    if (server->peer_count >= server->peer_capacity) {
        server->peer_capacity = server->peer_capacity ? server->peer_capacity * 2 : 10;
        server->peers = realloc(server->peers, server->peer_capacity * sizeof(*server->peers));
        if (!server->peers) abort();
    }
    server->peers[server->peer_count++] = peer;
}

struct chat_server*
chat_server_new(void)
{
    struct chat_server *server = calloc(1, sizeof(*server));
    if (!server) abort();
    server->socket = -1;
    server->kq = -1;
    server->peer_capacity = 0;
    server->peer_count = 0;
    server->peers = NULL;
    return server;
}

void
chat_server_delete(struct chat_server *server)
{
    if (server->socket >= 0) close(server->socket);
    if (server->kq >= 0) close(server->kq);
    for (size_t i = 0; i < server->peer_count; ++i) {
        peer_delete(server->peers[i]);
    }
    free(server->peers);
    free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
    server->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket < 0) return CHAT_ERR_SYS;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(server->socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->socket);
        return CHAT_ERR_PORT_BUSY;
    }

    if (listen(server->socket, SOMAXCONN) < 0) {
        close(server->socket);
        return CHAT_ERR_SYS;
    }

    server->kq = kqueue();
    if (server->kq < 0) {
        close(server->socket);
        return CHAT_ERR_SYS;
    }

    struct kevent ev;
    EV_SET(&ev, server->socket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(server->kq, &ev, 1, NULL, 0, NULL) < 0) {
        close(server->socket);
        close(server->kq);
        return CHAT_ERR_SYS;
    }
    return 0;
}

static void
handle_new_connection(struct chat_server *server)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_sock = accept(server->socket, (struct sockaddr*)&client_addr, &addr_len);
    if (client_sock < 0) {
        return;
    }

    fcntl(client_sock, F_SETFL, O_NONBLOCK);

    struct kevent ev;
    EV_SET(&ev, client_sock, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void*)(intptr_t)client_sock);
    if (kevent(server->kq, &ev, 1, NULL, 0, NULL) < 0) {
        close(client_sock);
        return;
    }

    struct chat_peer *peer = peer_new(client_sock);
    server_add_peer(server, peer);
}

static void
broadcast(struct chat_server *server, const char *msg, size_t len, struct chat_peer *sender)
{
    for (size_t i = 0; i < server->peer_count; ++i) {
        struct chat_peer *peer = server->peers[i];
        if (peer == sender || peer->socket < 0) continue;

        size_t new_size = peer->out_buf_size + len;
        char *new_buf = realloc(peer->out_buf, new_size);
        if (!new_buf) continue;
        peer->out_buf = new_buf;

        memcpy(peer->out_buf + peer->out_buf_size, msg, len);
        peer->out_buf_size = new_size;

        struct kevent ev;
        EV_SET(&ev, peer->socket, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, (void*)(intptr_t)peer->socket);
        kevent(server->kq, &ev, 1, NULL, 0, NULL);
    }
}

static void
process_client_input(struct chat_server *server, struct chat_peer *peer)
{
    char buf[BUFFER_SIZE];
    ssize_t n = recv(peer->socket, buf, sizeof(buf), 0);

    if (n <= 0) {
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(peer->socket);
            peer->socket = -1;
        }
        return;
    }

    // Добавляем полученные данные в in_buf
    size_t required_size = peer->in_buf_pos + n;
    if (required_size > peer->in_buf_size) {
        size_t new_buf_size = peer->in_buf_size == 0 ? BUFFER_SIZE : peer->in_buf_size * 2;
        while (new_buf_size < required_size) {
            new_buf_size *= 2;
        }
        char *new_buf = realloc(peer->in_buf, new_buf_size);
        if (!new_buf) abort();
        peer->in_buf = new_buf;
        peer->in_buf_size = new_buf_size;
    }
    memcpy(peer->in_buf + peer->in_buf_pos, buf, n);
    peer->in_buf_pos += n;

    // Обрабатываем полные сообщения
    char *start = peer->in_buf;
    char *end;
    while ((end = memchr(start, '\n', peer->in_buf_pos - (start - peer->in_buf)))) {
        size_t len = end - start;

        struct chat_message *msg = malloc(sizeof(*msg));
        if (!msg) abort();
        msg->data = malloc(len + 1);
        if (!msg->data) {
            free(msg);
            abort();
        }
        memcpy(msg->data, start, len);
        msg->data[len] = '\0';
        msg->next = NULL;

        if (!server->messages) {
            server->messages = msg;
        } else {
            struct chat_message *cur = server->messages;
            while (cur->next) cur = cur->next;
            cur->next = msg;
        }

        char *broadcast_msg = malloc(len + 1);
        if (!broadcast_msg) abort();
        memcpy(broadcast_msg, start, len);
        broadcast_msg[len] = '\n';
        broadcast(server, broadcast_msg, len + 1, peer);
        free(broadcast_msg);

        start = end + 1;
    }

    // Удаляем обработанные данные из in_buf
    size_t remaining = peer->in_buf_pos - (start - peer->in_buf);
    if (remaining > 0) {
        memmove(peer->in_buf, start, remaining);
    }
    peer->in_buf_pos = remaining;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
    if (server->socket < 0) {
        return CHAT_ERR_NOT_STARTED;
    }

    struct kevent events[MAX_EVENTS];
    struct timespec ts = {
        .tv_sec = (time_t)timeout,
        .tv_nsec = (long)((timeout - (int)timeout) * 1e9)
    };

    int processed = 0; // Флаг обработки событий или отключений
    int n = kevent(server->kq, NULL, 0, events, MAX_EVENTS, timeout < 0 ? NULL : &ts);
    if (n < 0) {
        return CHAT_ERR_SYS;
    }

    if (n > 0) {
        processed = 1; // Были обработаны события
    }

    for (int i = 0; i < n; ++i) {
        struct kevent *ev = &events[i];
        if ((int)ev->ident == server->socket) {
            handle_new_connection(server);
        } else {
            struct chat_peer *peer = NULL;
            for (size_t j = 0; j < server->peer_count; ++j) {
                if (server->peers[j]->socket == (int)ev->ident) {
                    peer = server->peers[j];
                    break;
                }
            }
            if (peer) {
                if (ev->filter == EVFILT_READ) {
                    if (ev->flags & EV_EOF) {
                        close(peer->socket);
                        peer->socket = -1;
                    } else {
                        process_client_input(server, peer);
                    }
                } else if (ev->filter == EVFILT_WRITE) {
                    if (peer->out_buf_pos < peer->out_buf_size) {
                        ssize_t sent = send(peer->socket, peer->out_buf + peer->out_buf_pos,
                                            peer->out_buf_size - peer->out_buf_pos, 0);
                        if (sent > 0) {
                            peer->out_buf_pos += sent;
                            if (peer->out_buf_pos >= peer->out_buf_size) {
                                peer->out_buf_pos = 0;
                                peer->out_buf_size = 0;
                                free(peer->out_buf);
                                peer->out_buf = NULL;
                                struct kevent ev_del;
                                EV_SET(&ev_del, peer->socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                                kevent(server->kq, &ev_del, 1, NULL, 0, NULL);
                            }
                        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            close(peer->socket);
                            peer->socket = -1;
                        }
                    }
                }
                // Удаляем отключенного клиента
                if (peer->socket < 0) {
                    for (size_t j = 0; j < server->peer_count; ++j) {
                        if (server->peers[j] == peer) {
                            peer_delete(peer);
                            server->peers[j] = server->peers[--server->peer_count];
                            break;
                        }
                    }
                }
            }
        }
    }

    // Проверяем и удаляем отключенных клиентов, даже если событий не было
    for (size_t j = 0; j < server->peer_count; ) {
        if (server->peers[j]->socket < 0) {
            peer_delete(server->peers[j]);
            server->peers[j] = server->peers[--server->peer_count];
            processed = 1; // Отмечаем, что обработали отключение
        } else {
            ++j;
        }
    }

    return processed ? 0 : CHAT_ERR_TIMEOUT; // Возвращаем 0, если были события или удалены клиенты
}

struct chat_message*
chat_server_pop_next(struct chat_server *server)
{
    if (!server->messages) return NULL;
    struct chat_message *msg = server->messages;
    server->messages = msg->next;
    msg->next = NULL;
    return msg;
}

int
chat_server_get_socket(const struct chat_server *server)
{
    return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
    if (server->socket < 0) {
        return 0;
    }
    int events = CHAT_EVENT_INPUT;
    for (size_t i = 0; i < server->peer_count; ++i) {
        if (server->peers[i]->out_buf_size > server->peers[i]->out_buf_pos) {
            events |= CHAT_EVENT_OUTPUT;
            break;
        }
    }
    return events;
}