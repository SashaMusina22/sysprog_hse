#include "parser.h"
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    char *buffer;
    uint32_t size;
    uint32_t capacity;
};

enum token_type {
    TOKEN_TYPE_NONE,
    TOKEN_TYPE_STR,
    TOKEN_TYPE_NEW_LINE,
    TOKEN_TYPE_PIPE,
    TOKEN_TYPE_AND,
    TOKEN_TYPE_OR,
    TOKEN_TYPE_OUT_NEW,
    TOKEN_TYPE_OUT_APPEND,
    TOKEN_TYPE_BACKGROUND
};

struct token {
    enum token_type type;
    char *data;
    uint32_t size;
    uint32_t capacity;
};

static char *token_strdup(const struct token *t) {
    assert(t->type == TOKEN_TYPE_STR && t->size > 0);
    char *res = malloc(t->size + 1);
    memcpy(res, t->data, t->size);
    res[t->size] = 0;
    return res;
}

static void token_append(struct token *t, char c) {
    if (t->size == t->capacity) {
        t->capacity = (t->capacity + 1) * 2;
        t->data = realloc(t->data, sizeof(*t->data) * t->capacity);
    }
    t->data[t->size++] = c;
}

static void token_reset(struct token *t) {
    t->size = 0;
    t->type = TOKEN_TYPE_NONE;
}

static void command_append_arg(struct command *cmd, char *arg) {
    if (cmd->arg_count == cmd->arg_capacity) {
        cmd->arg_capacity = (cmd->arg_capacity + 1) * 2;
        cmd->args = realloc(cmd->args, sizeof(*cmd->args) * cmd->arg_capacity);
    }
    cmd->args[cmd->arg_count++] = arg;
}

void command_line_delete(struct command_line *line) {
    while (line->head) {
        struct expr *e = line->head;
        if (e->type == EXPR_TYPE_COMMAND) {
            struct command *cmd = &e->cmd;
            free(cmd->exe);
            for (uint32_t i = 0; i < cmd->arg_count; ++i)
                free(cmd->args[i]);
            free(cmd->args);
        }
        line->head = e->next;
        free(e);
    }
    free(line->out_file);
    free(line);
}

static uint32_t parse_token(const char *pos, const char *end, struct token *out) {
    token_reset(out);
    const char *begin = pos;
    while (pos < end) {
        if (!isspace(*pos)) break;
        if (*pos == '\n') {
            out->type = TOKEN_TYPE_NEW_LINE;
            return pos + 1 - begin;
        }
        ++pos;
    }

    char quote = 0;
    while (pos < end) {
        char c = *pos;
        switch (c) {
            case '\'':
            case '"':
                if (!quote) { quote = c; ++pos; if (pos == end) return 0; continue; }
                if (quote != c) goto append_and_next;
                out->type = TOKEN_TYPE_STR;
                return pos + 1 - begin;
            case '\\':
                if (quote == '\'') goto append_and_next;
                if (quote == '"') {
                    ++pos; if (pos == end) return 0;
                    c = *pos;
                    if (c == '\\' || c == '"' || c == '\n') { token_append(out, c); ++pos; continue; }
                    token_append(out, '\\'); goto append_and_next;
                }
                ++pos; if (pos == end) return 0;
                c = *pos; if (c == '\n') { ++pos; continue; }
                goto append_and_next;
            case '&': case '|': case '>':
                if (quote) goto append_and_next;
                if (out->size > 0) { out->type = TOKEN_TYPE_STR; return pos - begin; }
                ++pos; if (pos == end) return 0;
                if (*pos == c) {
                    out->type = (c == '&') ? TOKEN_TYPE_AND : (c == '|') ? TOKEN_TYPE_OR : TOKEN_TYPE_OUT_APPEND;
                    ++pos;
                } else {
                    out->type = (c == '&') ? TOKEN_TYPE_BACKGROUND : (c == '|') ? TOKEN_TYPE_PIPE : TOKEN_TYPE_OUT_NEW;
                }
                return pos - begin;
            case ' ': case '\t': case '\r':
                if (!quote) { out->type = TOKEN_TYPE_STR; return pos + 1 - begin; }
                goto append_and_next;
            case '\n':
                if (!quote) { out->type = TOKEN_TYPE_STR; return pos - begin; }
                goto append_and_next;
            case '#':
                if (!quote) {
                    if (out->size > 0) { out->type = TOKEN_TYPE_STR; return pos - begin; }
                    ++pos;
                    while (pos < end) {
                        if (*pos == '\n') { out->type = TOKEN_TYPE_NEW_LINE; return pos + 1 - begin; }
                        ++pos;
                    }
                    return 0;
                }
                goto append_and_next;
            default: goto append_and_next;
        }
    append_and_next:
        token_append(out, c);
        ++pos;
    }
    return 0;
}

struct parser *parser_new(void) {
    return calloc(1, sizeof(struct parser));
}

void parser_feed(struct parser *p, const char *str, uint32_t len) {
    uint32_t cap = p->capacity - p->size;
    if (cap < len) {
        uint32_t new_capacity = (p->capacity + 1) * 2;
        if (new_capacity - p->size < len)
            new_capacity = p->size + len;
        p->buffer = realloc(p->buffer, sizeof(*p->buffer) * new_capacity);
        p->capacity = new_capacity;
    }
    memcpy(p->buffer + p->size, str, len);
    p->size += len;
}

static void parser_consume(struct parser *p, uint32_t size) {
    if (size == p->size) { p->size = 0; return; }
    memmove(p->buffer, p->buffer + size, p->size - size);
    p->size -= size;
}

enum parser_error parser_pop_next(struct parser *p, struct command_line **out) {
    struct command_line *line = calloc(1, sizeof(*line));
    char *pos = p->buffer;
    const char *begin = pos;
    char *end = pos + p->size;
    struct token token = {0};

    while (pos < end) {
        uint32_t used = parse_token(pos, end, &token);
        if (used == 0) goto no_line;
        pos += used;
        struct expr *e;
        switch (token.type) {
            case TOKEN_TYPE_STR:
                if (line->tail && line->tail->type == EXPR_TYPE_COMMAND) {
                    command_append_arg(&line->tail->cmd, token_strdup(&token));
                    continue;
                }
                e = calloc(1, sizeof(*e));
                e->type = EXPR_TYPE_COMMAND;
                e->cmd.exe = token_strdup(&token);
                if (!line->head)
                    line->head = e;
                else
                    line->tail->next = e;
                line->tail = e;
                continue;
            case TOKEN_TYPE_NEW_LINE:
                if (!line->tail) continue;
                goto finished;
            case TOKEN_TYPE_PIPE: case TOKEN_TYPE_AND: case TOKEN_TYPE_OR: {
                if (!line->tail) return token.type == TOKEN_TYPE_PIPE ? PARSER_ERR_PIPE_WITH_NO_LEFT_ARG :
                                         token.type == TOKEN_TYPE_AND ? PARSER_ERR_AND_WITH_NO_LEFT_ARG : PARSER_ERR_OR_WITH_NO_LEFT_ARG;
                if (line->tail->type != EXPR_TYPE_COMMAND)
                    return token.type == TOKEN_TYPE_PIPE ? PARSER_ERR_PIPE_WITH_LEFT_ARG_NOT_A_COMMAND :
                           token.type == TOKEN_TYPE_AND ? PARSER_ERR_AND_WITH_LEFT_ARG_NOT_A_COMMAND : PARSER_ERR_OR_WITH_LEFT_ARG_NOT_A_COMMAND;
                e = calloc(1, sizeof(*e));
                e->type = (token.type == TOKEN_TYPE_PIPE) ? EXPR_TYPE_PIPE :
                          (token.type == TOKEN_TYPE_AND) ? EXPR_TYPE_AND : EXPR_TYPE_OR;
                line->tail->next = e;
                line->tail = e;
                continue;
            }
            case TOKEN_TYPE_OUT_NEW: case TOKEN_TYPE_OUT_APPEND: case TOKEN_TYPE_BACKGROUND: goto finished;
            default: assert(false);
        }
    }
no_line:
    free(line);
    *out = NULL;
    return PARSER_ERR_NONE;

finished:
    if (token.type == TOKEN_TYPE_OUT_NEW || token.type == TOKEN_TYPE_OUT_APPEND) {
        line->out_type = (token.type == TOKEN_TYPE_OUT_NEW) ? OUTPUT_TYPE_FILE_NEW : OUTPUT_TYPE_FILE_APPEND;
        uint32_t u = parse_token(pos, end, &token);
        if (u == 0) goto no_line;
        pos += u;
        if (token.type != TOKEN_TYPE_STR) return PARSER_ERR_OUTOUT_REDIRECT_BAD_ARG;
        line->out_file = token_strdup(&token);
        u = parse_token(pos, end, &token);
        if (u == 0) goto no_line;
        pos += u;
    }
    if (token.type == TOKEN_TYPE_BACKGROUND) {
        line->is_background = true;
        uint32_t u = parse_token(pos, end, &token);
        if (u == 0) goto no_line;
        pos += u;
    }
    if (token.type == TOKEN_TYPE_NEW_LINE) {
        parser_consume(p, pos - begin);
        *out = line;
        return PARSER_ERR_NONE;
    }
    return PARSER_ERR_TOO_LATE_ARGUMENTS;
}

void parser_delete(struct parser *p) {
    free(p->buffer);
    free(p);
}
