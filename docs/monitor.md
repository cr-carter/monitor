# Required `exec` Commands

Your `monitor` program **must** use `fork()` + an `exec*()` function (e.g., `execvp`, `execlp`) to run **all** of the following commands:

1. `uptime`  
   - Use this to obtain the **1, 5, and 15 minute load averages**.  
   - You must capture its standard output via a pipe and parse those three numbers.

2. `free -b`  
   - Use this to obtain **total, used, and free memory in bytes**.  
   - You must capture its standard output via a pipe and parse the `Mem:` line.

3. `df -B1 /`  
   - Use this to obtain **disk usage for the root filesystem (`/`) in bytes**: total, used, available, and used percentage.  
   - You must capture its standard output via a pipe and parse the line corresponding to `/`.

4. `ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head`  
   - Use this to obtain information about the **top CPU-using processes**.  
   - You must capture its standard output via a pipe and parse at least:
     - The PID, command name, and `%CPU` for the first few processes (for example, the top 3).  

---

## Sampling Interval Requirement

- Your `monitor` program must run its full collection cycle (all of the above commands) at **fixed 5-second intervals**.
- That is, approximately every 5 seconds your program should:
  - Run all four commands via `fork()` + `exec*()`.
  - Capture and parse their output.
  - Log one complete sample of metrics.
  - Update whatever live dashboard or status display you provide.

---

**Constraints:**

- All four of the above commands **must** be started using `fork()` and an `exec`-family call (`execvp`, `execlp`, etc.).  
- For **each** of the four commands, you must:
  - Create a pipe.
  - Redirect the child’s `stdout` to the pipe using `dup2`.
  - Read the command output in the parent.
  - Parse the text into numeric/structured metrics.

## Bonus (Optional)

- For **bonus credit**, before printing the updated metrics for each 5-second sample, **clear the screen** so that only the most recent snapshot of system health is visible.
- This can be done in any reasonable way (for example, printing an ANSI clear-screen escape sequence or calling an appropriate library function), as long as:
  - The screen is visibly refreshed each time.
  - Old samples are not left cluttering the display.