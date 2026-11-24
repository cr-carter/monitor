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

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Application-specific exit codes for diagnosing setup and signal errors
#define EXIT_PIPE_FAILURE 10
#define EXIT_CLOSE_FAILURE 11
#define EXIT_DUP2_FAILURE 12
#define EXIT_SIGEMPTYSET_FAILURE 13

// Named indices for the two ends of an unnamed pipe
#define PIPE_READ 0
#define PIPE_WRITE 1

// Sleep system call
#define SLEEP_INTERVAL 5

// Maximum line buffer size (including null terminator)
#define MAX_LINE_LEN 256

// Set to 1 when the parent receives SIGINT (Ctrl+C) so the loop can wind down cleanly
volatile sig_atomic_t interrupt_flag = 0;

// Static functions
static void do_uptime(int uptime_pipe_fd[2]);
static void do_free(int free_pipe_fd[2], int uptime_pipe_fd[2]);
static void do_df(int df_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2]);
static void do_ps(int ps_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2], int df_pipe_fd[2]);
static void do_head(int head_pipe_fd[2], int ps_pipe_fd[2]);

/**
 * @brief Async-signal-safe handler for SIGINT in the parent process.
 * Only modifies a volatile sig_atomic_t flag; avoids non-reentrant calls like printf().
 */
static void parent_sigint_handler(int sig)
{
    if (SIGINT == sig)
    {
        interrupt_flag = 1;
    }
}

/**
 * Helper to close file descriptors
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
 * Helper to close pipe
 */
static void close_pipe(int pipe_fd[2])
{
    close_fd(pipe_fd[PIPE_READ]);
    close_fd(pipe_fd[PIPE_WRITE]);
}

/**
 * Child 1: uptime
 *      - Obtains the 1, 5, and 15 minute load averages
 *      - Capture its standard output via a pipe and parse those three numbers
 */
static void do_uptime(int uptime_pipe_fd[2])
{
    // Reset SIGINT dfault action
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(SIGINT, &default_action, NULL);

    // Flush stdout prior to fd changing
    fflush(stdout);

    // Child only writes to the pipe; close the unused fd immediately
    close_fd(uptime_pipe_fd[PIPE_READ]);

    // Duplicate pipe write to stdout
    if (-1 == dup2(uptime_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        _exit(EXIT_DUP2_FAILURE);
    }
    close_fd(uptime_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"uptime", NULL};
    execvp("uptime", args);

    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 2: free -b
 *      - Obtain total, used, and free memory in bytes
 *      - Capture its standard output via a pipe and parse the 'Mem:' line
 */
static void do_free(int free_pipe_fd[2], int uptime_pipe_fd[2])
{
    // Reset SIGINT dfault action
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(SIGINT, &default_action, NULL);

    // Flush stdout prior to fd changing
    fflush(stdout);

    // Child only writes to the pipe; close the unused fd immediately
    close_fd(free_pipe_fd[PIPE_READ]);
    close_pipe(uptime_pipe_fd);

    // Duplicate pipe write to stdout
    if (-1 == dup2(free_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        _exit(EXIT_DUP2_FAILURE);
    }
    close_fd(free_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"free", (char *)"-b", NULL};
    execvp("free", args);

    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 3: df -B1 /
 *      - Obtain disk usage for the root filesystem in bytes: total, used, available, and used percentage
 *      - Capture its standard output via a pipe and parse the line corresponding to '/'
 */
static void do_df(int df_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2])
{
    // Reset SIGINT dfault action
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(SIGINT, &default_action, NULL);

    // Flush stdout prior to fd changing
    fflush(stdout);

    // Child only writes to the pipe; close the unused fd immediately
    close_fd(df_pipe_fd[PIPE_READ]);
    close_pipe(uptime_pipe_fd);
    close_pipe(free_pipe_fd);

    // Duplicate pipe write to stdout
    if (-1 == dup2(df_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        _exit(EXIT_DUP2_FAILURE);
    }
    close_fd(df_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"df", (char *)"-B1", (char *)"/", NULL};
    execvp("df", args);

    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 4: ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head
 *      - Obtain information about the top CPU-using processes
 *      - Capture its standard output via a pipe and parse: the PID, command name, %CPU for the first 3 processes
 */
static void do_ps(int ps_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2], int df_pipe_fd[2])
{
    // Reset SIGINT dfault action
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(SIGINT, &default_action, NULL);

    // Flush stdout prior to fd changing
    fflush(stdout);

    // Child only writes to the pipe; close the unused fd immediately
    close_fd(ps_pipe_fd[PIPE_READ]);
    close_pipe(uptime_pipe_fd);
    close_pipe(free_pipe_fd);
    close_pipe(df_pipe_fd);

    int head_pipe_fd[2] = {-1, -1};
    // Create the unnamed pipe; O_CLOEXEC ensures descriptors close if exec() is ever used
    if (-1 == pipe2(head_pipe_fd, O_CLOEXEC))
    {
        perror("pipe2");
        _exit(EXIT_PIPE_FAILURE);
    }

    // Create the head child
    pid_t head_pid = fork();

    if (-1 == head_pid)
    {
        perror("fork");

        // Clean up both pipe ends since no child exists to share them
        close_pipe(head_pipe_fd);

        _exit(EXIT_FAILURE);
    }
    if (0 == head_pid)
    {
        // Child executes exits via _exit()
        do_head(head_pipe_fd, ps_pipe_fd);

        // Defensive fallback: child functions should not return.
        _exit(EXIT_FAILURE);
    }

    // PS does not need these
    close_fd(head_pipe_fd[PIPE_READ]);
    if (-1 == dup2(head_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("dup2");
        _exit(EXIT_DUP2_FAILURE);
    }

    close_fd(head_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"ps", (char *)"-eo", (char *)"pid,comm,%cpu,%mem", (char *)"--sort=-%cpu", NULL};
    execvp("ps", args);

    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Helper for ps, used to pass ps results into head
 */
static void do_head(int head_pipe_fd[2], int ps_pipe_fd[2])
{
    // Head does not write into it's own pipe
    close_fd(head_pipe_fd[PIPE_WRITE]);

    // Head reads from ps
    if (-1 == dup2(head_pipe_fd[PIPE_READ], STDIN_FILENO))
    {
        perror("dup2");
        _exit(EXIT_DUP2_FAILURE);
    }

    // Close original descriptor after duplication.
    close_fd(head_pipe_fd[PIPE_READ]);

    // Head writes to the original parent's pipe
    if (-1 == dup2(ps_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("dup2");
        _exit(EXIT_DUP2_FAILURE);
    }

    // Close original descriptor after duplication
    close_fd(ps_pipe_fd[PIPE_WRITE]);

    char *args[] = {(char *)"head", (char *)"-n", (char *)"4", NULL};
    execvp("head", args);

    perror("execvp head");
    _exit(EXIT_FAILURE);
}

/**
 * Parent
 *      - Read the command output
 *      - Parse the text into numeric/structured metrics
 */
static int do_parent(int uptime_pid, int uptime_pipe_fd[2], int free_pid, int free_pipe_fd[2], int df_pid,
                     int df_pipe_fd[2], int ps_pid, int ps_pipe_fd[2])
{
    int retval = EXIT_SUCCESS;

    printf("I am the parent (PID %d). (Ctrl+C to quit)\n\n", (int)getpid());
    fflush(stdout);

    // Parent only reads to the pipe; close the unused fd immediately
    close_fd(uptime_pipe_fd[PIPE_WRITE]);
    close_fd(free_pipe_fd[PIPE_WRITE]);
    close_fd(df_pipe_fd[PIPE_WRITE]);
    close_fd(ps_pipe_fd[PIPE_WRITE]);

    int status;

    // Setup and call uptime
    FILE *pipe_stream = fdopen(uptime_pipe_fd[PIPE_READ], "r");
    char line[MAX_LINE_LEN];

    while (fgets(line, sizeof(line), pipe_stream) != NULL)
    {
        printf("%s", line);
    }
    fclose(pipe_stream);
    if (-1 == waitpid(uptime_pid, &status, 0))
    {
        perror("waitpid");
        retval = EXIT_FAILURE;
    }
    puts("");

    // Setup and call free
    pipe_stream = fdopen(free_pipe_fd[PIPE_READ], "r");
    while (fgets(line, sizeof(line), pipe_stream) != NULL)
    {
        printf("%s", line);
    }
    fclose(pipe_stream);
    if (-1 == waitpid(free_pid, &status, 0))
    {
        perror("waitpid");
        retval = EXIT_FAILURE;
    }
    puts("");

    // Setup and call df
    pipe_stream = fdopen(df_pipe_fd[PIPE_READ], "r");
    while (fgets(line, sizeof(line), pipe_stream) != NULL)
    {
        printf("%s", line);
    }
    fclose(pipe_stream);
    if (-1 == waitpid(df_pid, &status, 0))
    {
        perror("waitpid");
        retval = EXIT_FAILURE;
    }
    puts("");

    // Setup and call ps
    pipe_stream = fdopen(ps_pipe_fd[PIPE_READ], "r");
    while (fgets(line, sizeof(line), pipe_stream) != NULL)
    {
        printf("%s", line);
    }
    fclose(pipe_stream);
    if (-1 == waitpid(ps_pid, &status, 0))
    {
        perror("waitpid");
        retval = EXIT_FAILURE;
    }
    puts("");

    return retval;
}

int main()
{
    int retval = EXIT_SUCCESS;

    // Register SIGINT handler for the parent (activated once unblocked in do_parent())
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

    // Process identifier, pipe descriptors
    int uptime_pipe_fd[2] = {-1, -1};
    int free_pipe_fd[2] = {-1, -1};
    int df_pipe_fd[2] = {-1, -1};
    int ps_pipe_fd[2] = {-1, -1};

    for (int i = 0;; i++)
    {
        if (1 == interrupt_flag)
        {
            printf("\n\n******************************\n\n");
            printf("Caught Ctrl+C\n");
            printf("\n******************************\n\n\n");
            break;
        }

        // system("clear");
        printf("\nIteration %d\n", i);

        // Create the unnamed pipe; O_CLOEXEC ensures descriptors close if exec() is ever used
        if (-1 == pipe2(uptime_pipe_fd, O_CLOEXEC))
        {
            perror("pipe2");
            return EXIT_PIPE_FAILURE;
        }

        // Flush buffered stdout prior to fork() so queued bytes aren't duplicated in both processes
        fflush(stdout);

        // Create the uptime child
        pid_t uptime_pid = fork();

        if (-1 == uptime_pid)
        {
            perror("fork");

            // Clean up both pipe ends since no child exists to share them
            close_pipe(uptime_pipe_fd);

            return EXIT_FAILURE;
        }
        if (0 == uptime_pid)
        {
            printf("'uptime' captured (PID %d).\n", (int)getpid());

            // Child executes exits via _exit()
            do_uptime(uptime_pipe_fd);
        }

        // Create the unnamed pipe; O_CLOEXEC ensures descriptors close if exec() is ever used
        if (-1 == pipe2(free_pipe_fd, O_CLOEXEC))
        {
            perror("pipe2");
            return EXIT_PIPE_FAILURE;
        }

        // Create the free child
        pid_t free_pid = fork();

        if (-1 == free_pid)
        {
            perror("fork");

            // Clean up both pipe ends since no child exists to share them
            close_pipe(free_pipe_fd);

            return EXIT_FAILURE;
        }
        if (0 == free_pid)
        {
            printf("'free -b' captured (PID %d).\n", (int)getpid());

            // Child executes exits via _exit()
            do_free(free_pipe_fd, uptime_pipe_fd);
        }

        // Create the unnamed pipe; O_CLOEXEC ensures descriptors close if exec() is ever used
        if (-1 == pipe2(df_pipe_fd, O_CLOEXEC))
        {
            perror("pipe2");
            return EXIT_PIPE_FAILURE;
        }

        // Create the df child
        pid_t df_pid = fork();

        if (-1 == df_pid)
        {
            perror("fork");

            // Clean up both pipe ends since no child exists to share them
            close_pipe(df_pipe_fd);

            return EXIT_FAILURE;
        }
        if (0 == df_pid)
        {
            printf("'df -B1 /' captured (PID %d).\n", (int)getpid());

            // Child executes exits via _exit()
            do_df(df_pipe_fd, uptime_pipe_fd, free_pipe_fd);
        }

        // Create the unnamed pipe; O_CLOEXEC ensures descriptors close if exec() is ever used
        if (-1 == pipe2(ps_pipe_fd, O_CLOEXEC))
        {
            perror("pipe2");
            return EXIT_PIPE_FAILURE;
        }

        // Create the ps child
        pid_t ps_pid = fork();

        if (-1 == ps_pid)
        {
            perror("fork");

            // Clean up both pipe ends since no child exists to share them
            close_pipe(ps_pipe_fd);

            return EXIT_FAILURE;
        }
        if (0 == ps_pid)
        {
            printf("'ps -eo pid,comm,%%cpu,%%mem --sort=-%%cpu | head -n 4' captured (PID %d).\n", (int)getpid());

            // Child executes exits via _exit()
            do_ps(ps_pipe_fd, uptime_pipe_fd, free_pipe_fd, df_pipe_fd);
        }

        sleep(SLEEP_INTERVAL);

        // Parent drives input, signaling, acknowledgment, and waitpid() cleanup
        retval = do_parent(uptime_pid, uptime_pipe_fd, free_pid, free_pipe_fd, df_pid, df_pipe_fd, ps_pid, ps_pipe_fd);
    }

    close_fd(STDIN_FILENO);
    close_fd(STDOUT_FILENO);
    close_fd(STDERR_FILENO);
    return retval;
}
