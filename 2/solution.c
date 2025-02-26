#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

static void handle_builtin_commands(const struct command *cmd) {
    if (strcmp(cmd->exe, "cd") == 0) {
        if (cmd->arg_count == 0) {
            fprintf(stderr, "Error! CD needs additional argument\n");
        } else if (chdir(cmd->args[0]) != 0) {
            perror("cd");
        }
        exit(0);
    }

    if (strcmp(cmd->exe, "exit") == 0) {
        exit(0);
    }
}

static void redirect_output(const struct command_line *line) {
    int fd;
    if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        fd = open(line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        fd = open(line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    } else {
        return;
    }

    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

static void execute_command(const struct command *cmd, const struct command_line *line) {
    if (!cmd->exe || cmd->exe[0] == '\0') {
        fprintf(stderr, "Error! Empty command\n");
        return;
    }

    handle_builtin_commands(cmd);
    redirect_output(line);

    char *args[cmd->arg_count + 2];
    args[0] = cmd->exe;
    for (uint32_t i = 0; i < cmd->arg_count; i++) {
        args[i + 1] = cmd->args[i];
    }
    args[cmd->arg_count + 1] = NULL;

    if (execvp(args[0], args) == -1) {
        perror("execvp");
        exit(EXIT_FAILURE);
    }
}

static void execute_pipeline(const struct command_line *line) {
    int prev_fd = -1, pipefd[2], last_status = 0;
    pid_t pid;
    enum expr_type last_type = EXPR_TYPE_COMMAND;

    for (const struct expr *expr = line->head; expr; expr = expr->next) {
        if ((last_type == EXPR_TYPE_AND && last_status != 0) ||
            (last_type == EXPR_TYPE_OR && last_status == 0)) {
            last_type = expr->type;
            continue;
        }

        if (expr->type == EXPR_TYPE_COMMAND) {
            int use_pipe = expr->next && expr->next->type == EXPR_TYPE_PIPE;
            if (use_pipe && pipe(pipefd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            pid = fork();
            if (pid == 0) { // Child process
                if (prev_fd != -1) {
                    dup2(prev_fd, STDIN_FILENO);
                    close(prev_fd);
                }
                if (use_pipe) {
                    close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    close(pipefd[1]);
                }
                execute_command(&expr->cmd, line);
            } else if (pid < 0) {
                perror("fork");
                exit(EXIT_FAILURE);
            }

            if (prev_fd != -1) {
                close(prev_fd);
            }
            if (use_pipe) {
                close(pipefd[1]);
                prev_fd = pipefd[0];
            }

            int status;
            waitpid(pid, &status, 0);
            last_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        last_type = expr->type;
    }
}

int main(void) {
    setbuf(stdout, NULL);
    struct parser *parser = parser_new();
    char buf[1024];
    int rc;

    while ((rc = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        parser_feed(parser, buf, rc);
        struct command_line *line = NULL;

        while (true) {
            enum parser_error err = parser_pop_next(parser, &line);
            if (err == PARSER_ERR_NONE && !line) break;
            if (err != PARSER_ERR_NONE) {
                fprintf(stderr, "Error: %d\n", err);
                continue;
            }
            execute_pipeline(line);
            command_line_delete(line);
        }
    }

    parser_delete(parser);
    return 0;
}

