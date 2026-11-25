/**
 * monitor program will run full collection cycle at fixed 5 second intervals
 *      - Run all four commands via fork() + exec*()
 *      - Capture and parse their output
 *      - Log one complete sample of metrics
 *      - Update whatever live dashboard or status display provided
 *
 * Each child:
 *      - fork()
 *      - Create a pipe
 *      - Redirect stdout to pipe
 *      - Read command output in parent
 *      - Parse text into numeric/structured metrics
 *
 * Child 1: uptime
 *      - Obtains the 1, 5, and 15 minute load averages
 *      - Capture its standard output via a pipe and parse those three numbers
 * Child 2: free -b
 *      - Obtain total, used, and free memory in bytes
 *      - Capture its standard output via a pipe and parse the 'Mem:' line
 * Child 3: df -B1 /
 *      - Obtain disk usage for the root filesystem in bytes: total, used, available, and used percentage
 *      - Capture its standard output via a pipe and parse the line corresponding to '/'
 * Child 4: ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head
 *      - Obtain information about the top CPU-using processes
 *      - Capture its standard output via a pipe and parse: the PID, command name, %CPU for the first 3 processes
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Application-specific exit codes */
#define EXIT_PIPE_FAILURE 10
#define EXIT_CLOSE_FAILURE 11
#define EXIT_DUP2_FAILURE 12
#define EXIT_SIGEMPTYSET_FAILURE 13

/* Named indices for the two ends of an unnamed pipe */
#define PIPE_READ 0
#define PIPE_WRITE 1

/* Sleep system call */
#define SLEEP_INTERVAL 5

/* Maximum line buffer size (including null terminator) */
#define MAX_LINE_LEN 256

/* Number of ps output lines to display: header + three processes */
#define PS_LINES_TO_DISPLAY 4

/* Set to 1 when the parent receives SIGINT */
volatile sig_atomic_t interrupt_flag = 0;

/* Static functions */
static void close_fd(int fd);
static void close_pipe(int pipe_fd[2]);
static void create_pipe(int pipe_fd[2]);
static void set_signal_action(int signal_number, void (*handler)(int));
static void redirect_stdout(int pipe_write_fd);
static void close_inherited_pipes(int *pipe_fds[], int count);
static void run_command(char *args[]);
static pid_t fork_command(void);
static int wait_for_child(pid_t command_pid, int *status);
static int print_output(pid_t command_pid, int command_pipe_read, char *command_str, int max_lines);
static int do_parent(int uptime_pid, int uptime_pipe_fd[2], int free_pid, int free_pipe_fd[2], int df_pid,
                     int df_pipe_fd[2], int ps_pid, int ps_pipe_fd[2]);

static void do_uptime(int uptime_pipe_fd[2]);
static void do_free(int free_pipe_fd[2], int uptime_pipe_fd[2]);
static void do_df(int df_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2]);
static void do_ps(int ps_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2], int df_pipe_fd[2]);

/**
 * @brief Async-signal-safe handler for SIGINT in the parent.
 */
static void parent_sigint_handler(int sig)
{
    if (SIGINT == sig)
    {
        interrupt_flag = 1;
    }
}

/**
 * Helper to close file descriptors.
 */
static void close_fd(int fd)
{
    if (-1 == close(fd))
    {
        perror("close");
        _exit(EXIT_CLOSE_FAILURE);
    }
}

/**
 * Helper to close pipe.
 */
static void close_pipe(int pipe_fd[2])
{
    close_fd(pipe_fd[PIPE_READ]);
    close_fd(pipe_fd[PIPE_WRITE]);
}

/**
 * Helper to create an unnamed pipe.
 *
 * O_CLOEXEC ensures descriptors close automatically if exec() succeeds.
 */
static void create_pipe(int pipe_fd[2])
{
    if (-1 == pipe2(pipe_fd, O_CLOEXEC))
    {
        perror("pipe2");
        _exit(EXIT_PIPE_FAILURE);
    }
}

/**
 * Helper to configure a signal action.
 */
static void set_signal_action(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = handler;

    if (-1 == sigemptyset(&action.sa_mask))
    {
        perror("sigemptyset");
        _exit(EXIT_SIGEMPTYSET_FAILURE);
    }

    action.sa_flags = 0;

    if (-1 == sigaction(signal_number, &action, NULL))
    {
        perror("sigaction");
        _exit(EXIT_FAILURE);
    }
}

/**
 * Helper to redirect stdout to a pipe.
 */
static void redirect_stdout(int pipe_write_fd)
{
    if (-1 == dup2(pipe_write_fd, STDOUT_FILENO))
    {
        perror("dup2");
        _exit(EXIT_DUP2_FAILURE);
    }

    close_fd(pipe_write_fd);
}

/**
 * Helper to close inherited descriptors belonging to other commands.
 *
 * The current command's pipe is deliberately excluded.
 */
static void close_inherited_pipes(int *pipe_fds[], int count)
{
    for (int i = 0; i < count; i++)
    {
        close_pipe(pipe_fds[i]);
    }
}

/**
 * Helper to execute a command.
 */
static void run_command(char *args[])
{
    execvp(args[0], args);

    perror("execvp");
    _exit(EXIT_FAILURE);
}

/**
 * Helper to fork a command.
 */
static pid_t fork_command(void)
{
    pid_t command_pid = fork();

    if (-1 == command_pid)
    {
        perror("fork");
        _exit(EXIT_FAILURE);
    }

    return command_pid;
}

/**
 * Helper to wait for a specific child process.
 *
 * Retries waitpid() if interrupted by a signal.
 */
static int wait_for_child(pid_t command_pid, int *status)
{
    pid_t result;

    do
    {
        result = waitpid(command_pid, status, 0);
    } while (-1 == result && EINTR == errno);

    if (-1 == result)
    {
        perror("waitpid");
        return EXIT_FAILURE;
    }

    if (!WIFEXITED(*status) || WEXITSTATUS(*status) != EXIT_SUCCESS)
    {
        fprintf(stderr, "Command (PID %d) failed\n", command_pid);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/**
 * Helper to print output from command pipe.
 *
 * max_lines:
 *      0 = display all output
 *      N = display only the first N lines
 *
 * IMPORTANT:
 *      Even when max_lines is reached, continue reading until EOF.
 *      This prevents the child from blocking when the pipe buffer fills.
 */
static int print_output(pid_t command_pid, int command_pipe_read, char *command_str, int max_lines)
{
    printf("%s\n", command_str);

    int status = 0;
    int retval = EXIT_SUCCESS;
    int lines_printed = 0;

    FILE *pipe_stream = fdopen(command_pipe_read, "r");

    if (NULL == pipe_stream)
    {
        perror("fdopen");
        close_fd(command_pipe_read);
        wait_for_child(command_pid, &status);
        return EXIT_FAILURE;
    }

    char line[MAX_LINE_LEN];

    for (;;)
    {
        errno = 0;

        if (NULL == fgets(line, sizeof(line), pipe_stream))
        {
            if (ferror(pipe_stream))
            {
                if (EINTR == errno)
                {
                    clearerr(pipe_stream);
                    continue;
                }

                perror("fgets");
                retval = EXIT_FAILURE;
            }

            break;
        }

        if (0 == max_lines || lines_printed < max_lines)
        {
            printf("%s", line);
            lines_printed++;
        }
    }

    if (EOF == fclose(pipe_stream))
    {
        perror("fclose");
        retval = EXIT_FAILURE;
    }

    /* Reap the command child after its output pipe reaches EOF */
    if (EXIT_SUCCESS != wait_for_child(command_pid, &status))
    {
        retval = EXIT_FAILURE;
    }

    puts("");

    return retval;
}

/**
 * Child 1: uptime
 *      - Obtains the 1, 5, and 15 minute load averages
 *      - Captures standard output via a pipe
 */
static void do_uptime(int uptime_pipe_fd[2])
{
    /* Ignore SIGINT */
    set_signal_action(SIGINT, SIG_IGN);

    /* Flush stdout prior to fd changing */
    fflush(stdout);

    /* Child only writes to the pipe; close the unused fd */
    close_fd(uptime_pipe_fd[PIPE_READ]);

    /* Duplicate pipe write to stdout */
    redirect_stdout(uptime_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"uptime", NULL};

    run_command(args);
}

/**
 * Child 2: free -b
 *      - Obtains total, used, and free memory in bytes
 *      - Captures standard output via a pipe
 */
static void do_free(int free_pipe_fd[2], int uptime_pipe_fd[2])
{
    /* Ignore SIGINT */
    set_signal_action(SIGINT, SIG_IGN);

    /* Flush stdout prior to fd changing */
    fflush(stdout);

    close_fd(free_pipe_fd[PIPE_READ]);

    /* Close inherited descriptors belonging to other commands */
    int *inherited_pipes[] = {uptime_pipe_fd};
    close_inherited_pipes(inherited_pipes, 1);

    redirect_stdout(free_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"free", (char *)"-b", NULL};
    run_command(args);
}

/**
 * Child 3: df -B1 /
 *      - Obtains disk usage for the root filesystem in bytes
 *      - Captures standard output via a pipe
 */
static void do_df(int df_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2])
{
    /* Ignore SIGINT */
    set_signal_action(SIGINT, SIG_IGN);

    /* Flush stdout prior to fd changing */
    fflush(stdout);

    close_fd(df_pipe_fd[PIPE_READ]);

    /* Close inherited descriptors belonging to other commands */
    int *inherited_pipes[] = {uptime_pipe_fd, free_pipe_fd};
    close_inherited_pipes(inherited_pipes, 2);

    redirect_stdout(df_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"df", (char *)"-B1", (char *)"/", NULL};

    run_command(args);
}

/**
 * Child 4: ps -eo pid,comm,%cpu,%mem --sort=-%cpu
 *      - Obtains information about the top CPU-using processes
 *      - Captures standard output via a pipe
 *
 * NOTE:
 *      head has been eliminated.
 *      The parent displays only the first three process records.
 */
static void do_ps(int ps_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2], int df_pipe_fd[2])
{
    /* Ignore SIGINT */
    set_signal_action(SIGINT, SIG_IGN);

    /* Flush stdout prior to fd changing */
    fflush(stdout);

    close_fd(ps_pipe_fd[PIPE_READ]);

    /* Close inherited descriptors belonging to other commands */
    int *inherited_pipes[] = {uptime_pipe_fd, free_pipe_fd, df_pipe_fd};
    close_inherited_pipes(inherited_pipes, 3);

    redirect_stdout(ps_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"ps", (char *)"-eo", (char *)"pid,comm,%cpu,%mem", (char *)"--sort=-%cpu", NULL};

    run_command(args);
}

/**
 * Parent
 *      - Reads the command output
 *      - Parses the text into numeric/structured metrics
 *      - Displays the first three ps process records
 *      - Waits for each command to terminate
 */
static int do_parent(int uptime_pid, int uptime_pipe_fd[2], int free_pid, int free_pipe_fd[2], int df_pid,
                     int df_pipe_fd[2], int ps_pid, int ps_pipe_fd[2])
{
    int retval = EXIT_SUCCESS;

    fflush(stdout);

    /* Parent only reads from the pipes; close unused write ends */
    close_fd(uptime_pipe_fd[PIPE_WRITE]);
    close_fd(free_pipe_fd[PIPE_WRITE]);
    close_fd(df_pipe_fd[PIPE_WRITE]);
    close_fd(ps_pipe_fd[PIPE_WRITE]);

    char command_str[MAX_LINE_LEN];

    /* Setup and call uptime */
    snprintf(command_str, sizeof(command_str), "'uptime' captured (PID %d).", uptime_pid);

    if (EXIT_SUCCESS != print_output(uptime_pid, uptime_pipe_fd[PIPE_READ], command_str, 0))
    {
        retval = EXIT_FAILURE;
    }

    /* Setup and call free */
    snprintf(command_str, sizeof(command_str), "'free -b' captured (PID %d).", free_pid);

    if (EXIT_SUCCESS != print_output(free_pid, free_pipe_fd[PIPE_READ], command_str, 0))
    {
        retval = EXIT_FAILURE;
    }

    /* Setup and call df */
    snprintf(command_str, sizeof(command_str), "'df -B1 /' captured (PID %d).", df_pid);

    if (EXIT_SUCCESS != print_output(df_pid, df_pipe_fd[PIPE_READ], command_str, 0))
    {
        retval = EXIT_FAILURE;
    }

    /* Setup and call ps */
    snprintf(command_str, sizeof(command_str), "'ps -eo pid,comm,%%cpu,%%mem --sort=-%%cpu' captured (PID %d).",
             ps_pid);

    if (EXIT_SUCCESS != print_output(ps_pid, ps_pipe_fd[PIPE_READ], command_str, PS_LINES_TO_DISPLAY))
    {
        retval = EXIT_FAILURE;
    }

    return retval;
}

int main(void)
{
    int retval = EXIT_SUCCESS;

    /* Register SIGINT handler for the parent */
    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = parent_sigint_handler;

    if (-1 == sigemptyset(&action.sa_mask))
    {
        perror("sigemptyset");
        return EXIT_SIGEMPTYSET_FAILURE;
    }

    action.sa_flags = 0;

    if (-1 == sigaction(SIGINT, &action, NULL))
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    /* Process identifiers and pipe descriptors */
    int uptime_pipe_fd[2] = {-1, -1};
    int free_pipe_fd[2] = {-1, -1};
    int df_pipe_fd[2] = {-1, -1};
    int ps_pipe_fd[2] = {-1, -1};

    int i = 0;

    while (0 == interrupt_flag)
    {
        system("clear");
        i++;

        printf("\n************\n");
        printf("Iteration %d\n", i);
        printf("************\n");

        /* Create the unnamed pipe */
        create_pipe(uptime_pipe_fd);

        fflush(stdout);

        /* Create the uptime child */
        pid_t uptime_pid = fork_command();

        if (0 == uptime_pid)
        {
            do_uptime(uptime_pipe_fd);
        }

        /* Create the unnamed pipe */
        create_pipe(free_pipe_fd);

        /* Create the free child */
        pid_t free_pid = fork_command();

        if (0 == free_pid)
        {
            do_free(free_pipe_fd, uptime_pipe_fd);
        }

        /* Create the unnamed pipe */
        create_pipe(df_pipe_fd);

        /* Create the df child */
        pid_t df_pid = fork_command();

        if (0 == df_pid)
        {
            do_df(df_pipe_fd, uptime_pipe_fd, free_pipe_fd);
        }

        /* Create the unnamed pipe */
        create_pipe(ps_pipe_fd);

        /* Create the ps child */
        pid_t ps_pid = fork_command();

        if (0 == ps_pid)
        {
            do_ps(ps_pipe_fd, uptime_pipe_fd, free_pipe_fd, df_pipe_fd);
        }

        /*
         * Parent drives input, signaling, acknowledgment,
         * and waitpid() cleanup.
         */
        retval = do_parent(uptime_pid, uptime_pipe_fd, free_pid, free_pipe_fd, df_pid, df_pipe_fd, ps_pid, ps_pipe_fd);

        /* Check if Ctrl+C was pressed before sleeping */
        if (0 != interrupt_flag)
        {
            break;
        }

        sleep(SLEEP_INTERVAL);

        /* Was Ctrl+C pressed during sleep? */
        if (0 != interrupt_flag)
        {
            break;
        }
    }

    printf("\n\n******************************\n\n");
    printf("Caught Ctrl+C\n");
    printf("\n******************************\n\n\n");

    return retval;
}

/* End of file monitor.c */
