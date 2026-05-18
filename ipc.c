#include "common.h"

#define PID_FILE "/tmp/copy_xlqd.pid"

/* ------------------------------------------------------------------ */
/*  signal handler – write byte to pipe so select() wakes up           */
/* ------------------------------------------------------------------ */

void handle_sigusr1(int sig)
{
    (void)sig;
    char b = 1;
    (void)!write(sig_pipe[1], &b, 1);
}

/* ------------------------------------------------------------------ */
/*  PID file helpers                                                   */
/* ------------------------------------------------------------------ */

int pidfile_write(void)
{
    int fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("open " PID_FILE);
        return -1;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, n) < 0)
    {
        perror("write " PID_FILE);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

void pidfile_remove(void)
{
    unlink(PID_FILE);
}

pid_t pidfile_read(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f)
        return -1;

    pid_t pid = -1;
    if (fscanf(f, "%d", &pid) != 1)
    {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (pid <= 0)
        return -1;

    if (kill(pid, 0) < 0)
    {
        if (errno == ESRCH)
        {
            fprintf(stderr, "info: stale PID file removed\n");
            unlink(PID_FILE);
        }
        return -1;
    }
    return pid;
}

void do_toggle_and_exit(void)
{
    pid_t pid = pidfile_read();
    if (pid < 0)
    {
        fprintf(stderr, "copy_xlqd: no running instance found (%s)\n",
                PID_FILE);
        exit(1);
    }
    if (kill(pid, SIGUSR1) < 0)
    {
        perror("kill SIGUSR1");
        exit(1);
    }
    printf("copy_xlqd: sent SIGUSR1 to pid %d\n", (int)pid);
    exit(0);
}
