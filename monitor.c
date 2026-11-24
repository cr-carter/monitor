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

#include <ctype.h>
#include <errno.h>
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
#define SLEEP_INTERVAL 1

// Set to 1 when the parent receives SIGINT (Ctrl+C) so the loop can wind down cleanly
volatile sig_atomic_t interrupt_flag = 0;

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
 * Child 1: uptime
 *      - Obtains the 1, 5, and 15 minute load averages
 *      - Capture its standard output via a pipe and parse those three numbers
 */
int do_uptime(int uptime_pipe_fd[2])
{
    int func_retval = EXIT_SUCCESS;

    printf("I am the child (PID %d).\n", (int)getpid());
    fflush(stdout);

    // Child only writes to the pipe; close the unused read end immediately
    if (-1 == close(uptime_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }

    printf("'uptime' captured\n");

    // Duplicate pipe write to stdout
    if (-1 == dup2(uptime_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        return EXIT_DUP2_FAILURE;
    }

    char *args[] = {"uptime", NULL};
    execvp("uptime", args);
    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 2: free -b
 *      - Obtain total, used, and free memory in bytes
 *      - Capture its standard output via a pipe and parse the 'Mem:' line
 */
int do_free(int free_pipe_fd[2], int uptime_pipe_fd[2])
{
    int func_retval = EXIT_SUCCESS;

    printf("I am the child (PID %d).\n", (int)getpid());
    fflush(stdout);

    // Child only writes to the pipe; close the unused read end immediately
    if (-1 == close(free_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }

    printf("'free -b' captured\n");

    // Duplicate pipe write to stdout
    if (-1 == dup2(free_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        return EXIT_DUP2_FAILURE;
    }

    char *args[] = {"free", "-b", NULL};
    execvp("free", args);
    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 3: df -B1 /
 *      - Obtain disk usage for the root filesystem in bytes: total, used, available, and used percentage
 *      - Capture its standard output via a pipe and parse the line corresponding to '/'
 */
int do_df(int df_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2])
{
    int func_retval = EXIT_SUCCESS;

    printf("I am the child (PID %d).\n", (int)getpid());
    fflush(stdout);

    // Child only writes to the pipe; close the unused read end immediately
    if (-1 == close(df_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(free_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(free_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }

    printf("'df -B1 /' captured\n");

    // Duplicate pipe write to stdout
    if (-1 == dup2(df_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        return EXIT_DUP2_FAILURE;
    }

    char *args[] = {"df", "-B1", "/", NULL};
    execvp("df", args);
    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Child 4: ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head
 *      - Obtain information about the top CPU-using processes
 *      - Capture its standard output via a pipe and parse: the PID, command name, %CPU for the first 3 processes
 */
int do_ps(int ps_pipe_fd[2], int uptime_pipe_fd[2], int free_pipe_fd[2], int df_pipe_fd[2])
{
    int func_retval = EXIT_SUCCESS;

    printf("I am the child (PID %d).\n", (int)getpid());
    fflush(stdout);

    // Child only writes to the pipe; close the unused read end immediately
    if (-1 == close(ps_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(uptime_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(free_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(free_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(df_pipe_fd[PIPE_READ]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(df_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
        return EXIT_CLOSE_FAILURE;
    }

    printf("'ps -eo pid,comm,%%cpu,%%mem --sort=-%%cpu | head' captured\n");

    // Duplicate pipe write to stdout
    if (-1 == dup2(ps_pipe_fd[PIPE_WRITE], STDOUT_FILENO))
    {
        perror("close");
        return EXIT_DUP2_FAILURE;
    }

    char *args[] = {"ps", "-eo", "pid,comm,%cpu,%mem", "--sort=-%cpu", NULL};
    //"| ", "head"
    execvp("ps", args);
    perror("execv failed");
    _exit(EXIT_FAILURE);
}

/**
 * Parent
 *      - Read the command output
 *      - Parse the text into numeric/structured metrics
 */
int do_parent(int uptime_pid, int uptime_pipe_fd[2], int free_pid, int free_pipe_fd[2], int df_pid, int df_pipe_fd[2],
              int ps_pid, int ps_pipe_fd[2])
{
    int retval = EXIT_SUCCESS;

    printf("I am the parent (PID %d). (Ctrl+C to quit)\n", (int)getpid());
    fflush(stdout);

    // Parent only reads to the pipe; close the unused write end immediately
    if (-1 == close(uptime_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
    }
    if (-1 == close(free_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
    }
    if (-1 == close(df_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
    }
    if (-1 == close(ps_pipe_fd[PIPE_WRITE]))
    {
        perror("close");
    }

    // Do stuff
    int status;

    if (-1 == waitpid(uptime_pid, &status, 0))
    {
        perror("waitpid");
    }
    printf("uptime PID: %d, status: %d\n", uptime_pid, status);
    if (-1 == waitpid(free_pid, &status, 0))
    {
        perror("waitpid");
    }
    printf("free PID: %d, status: %d\n", free_pid, status);
    if (-1 == waitpid(df_pid, &status, 0))
    {
        perror("waitpid");
    }
    printf("df PID: %d, status: %d\n", df_pid, status);
    if (-1 == waitpid(ps_pid, &status, 0))
    {
        perror("waitpid");
    }
    printf("ps PID: %d, status: %d\n", ps_pid, status);

    // Close the read end of the pipe
    if (-1 == close(uptime_pipe_fd[PIPE_READ]))
    {
        perror("close");
        retval = EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(free_pipe_fd[PIPE_READ]))
    {
        perror("close");
        retval = EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(df_pipe_fd[PIPE_READ]))
    {
        perror("close");
        retval = EXIT_CLOSE_FAILURE;
    }
    if (-1 == close(ps_pipe_fd[PIPE_READ]))
    {
        perror("close");
        retval = EXIT_CLOSE_FAILURE;
    }
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
    pid_t parent_pid = getpid();

    for (int i = 0;; i++)
    {
        if (1 == interrupt_flag)
        {
            printf("\n\n******************************\n\n");
            printf("Caught Ctrl+C\n");
            printf("\n******************************\n\n\n");
            break;
        }

        system("clear");
        printf("Iteration %d\n", i);

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
            close(uptime_pipe_fd[PIPE_READ]);
            close(uptime_pipe_fd[PIPE_WRITE]);

            return EXIT_FAILURE;
        }
        if (0 == uptime_pid)
        {
            // Child executes exits via _exit()
            retval = do_uptime(uptime_pipe_fd);

            // Defensive fallback: child functions should not return.
            _exit(EXIT_FAILURE);
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
            close(free_pipe_fd[PIPE_READ]);
            close(free_pipe_fd[PIPE_WRITE]);

            return EXIT_FAILURE;
        }
        if (0 == free_pid)
        {
            // Child executes exits via _exit()
            retval = do_free(free_pipe_fd, uptime_pipe_fd);

            // Defensive fallback: child functions should not return.
            _exit(EXIT_FAILURE);
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
            close(df_pipe_fd[PIPE_READ]);
            close(df_pipe_fd[PIPE_WRITE]);

            return EXIT_FAILURE;
        }
        if (0 == df_pid)
        {
            // Child executes exits via _exit()
            retval = do_df(df_pipe_fd, uptime_pipe_fd, free_pipe_fd);

            // Defensive fallback: child functions should not return.
            _exit(EXIT_FAILURE);
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
            close(ps_pipe_fd[PIPE_READ]);
            close(ps_pipe_fd[PIPE_WRITE]);

            return EXIT_FAILURE;
        }
        if (0 == ps_pid)
        {
            // Child executes exits via _exit()
            retval = do_ps(ps_pipe_fd, uptime_pipe_fd, free_pipe_fd, df_pipe_fd);

            // Defensive fallback: child functions should not return.
            _exit(EXIT_FAILURE);
        }

        sleep(SLEEP_INTERVAL);

        // Parent drives input, signaling, acknowledgment, and waitpid() cleanup
        retval = do_parent(uptime_pid, uptime_pipe_fd, free_pid, free_pipe_fd, df_pid, df_pipe_fd, ps_pid, ps_pipe_fd);
    }

    return retval;
}