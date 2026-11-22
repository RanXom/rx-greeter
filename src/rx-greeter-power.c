/*
 * rx-greeter-power: fixed-action privileged helper.
 *
 * This program intentionally accepts only suspend, poweroff, or reboot. It
 * never passes a string from the greeter UI to a shell. Install it either
 * setuid root (mode 4750, group greetd) or allow the greetd user to run it
 * through doas; when it is not root it re-executes itself through doas.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void
usage(void)
{
    fputs("usage: rx-greeter-power [--dry-run] [--no-doas] suspend|poweroff|reboot\n"
          "\n"
          "Environment:\n"
          "  RX_GREETER_POWER_DRY_RUN=1   print the action without performing it\n"
          "  RX_GREETER_POWER_NO_DOAS=1   never re-execute through doas\n",
          stderr);
}

static const char *
valid_action(const char *action)
{
    if (strcmp(action, "suspend") == 0 || strcmp(action, "poweroff") == 0 ||
        strcmp(action, "reboot") == 0) {
        return action;
    }
    return NULL;
}

static int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
wait_with_timeout(pid_t pid, int timeout_ms)
{
    int status = 0;
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (result < 0 && errno != EINTR) {
            return -1;
        }
        if (now_ms() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return -1;
        }
        usleep(10000);
    }
}

static int
try_loginctl(const char *action)
{
    const char *loginctl = NULL;
    if (access("/usr/bin/loginctl", X_OK) == 0) {
        loginctl = "/usr/bin/loginctl";
    } else if (access("/bin/loginctl", X_OK) == 0) {
        loginctl = "/bin/loginctl";
    }
    if (!loginctl) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        char *const envp[] = {
            (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            (char *)"LANG=C",
            NULL,
        };
        execle(loginctl, "loginctl", action, (char *)NULL, envp);
        _exit(127);
    }
    return wait_with_timeout(pid, 5000) == 0 ? 0 : -1;
}

static int
try_doas(const char *action)
{
    const char *doas = NULL;
    if (access("/usr/bin/doas", X_OK) == 0) {
        doas = "/usr/bin/doas";
    } else if (access("/usr/local/bin/doas", X_OK) == 0) {
        doas = "/usr/local/bin/doas";
    } else if (access("/bin/doas", X_OK) == 0) {
        doas = "/bin/doas";
    }
    if (!doas) {
        return -1;
    }

    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0) {
        return -1;
    }
    self[len] = '\0';

    if (getenv("RX_GREETER_POWER_NO_DOAS")) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl(doas, "doas", "-n", "-u", "root", self, action, (char *)NULL);
        _exit(127);
    }
    return wait_with_timeout(pid, 15000) == 0 ? 0 : -1;
}

static int
suspend_system(void)
{
    if (try_loginctl("suspend") == 0) {
        return 0;
    }

    int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("rx-greeter-power: open /sys/power/state");
        return 1;
    }
    const char *state = "mem\n";
    ssize_t written = write(fd, state, strlen(state));
    int saved = errno;
    close(fd);
    if (written < 0) {
        errno = saved;
        perror("rx-greeter-power: write /sys/power/state");
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    bool dry_run = getenv("RX_GREETER_POWER_DRY_RUN") != NULL;
    const char *action = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--no-doas") == 0) {
            setenv("RX_GREETER_POWER_NO_DOAS", "1", 1);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (!action && valid_action(argv[i])) {
            action = valid_action(argv[i]);
        } else {
            fprintf(stderr, "rx-greeter-power: invalid argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (!action) {
        usage();
        return 2;
    }

    if (dry_run) {
        printf("rx-greeter-power: dry-run: would %s\n", action);
        return 0;
    }

    if (geteuid() != 0) {
        int rc = try_doas(action);
        if (rc == 0) {
            return 0;
        }
        fprintf(stderr,
                "rx-greeter-power: not privileged; install it setuid root "
                "(mode 4750, group greetd) or add a doas rule for the greetd "
                "user\n");
        return 1;
    }

    if (strcmp(action, "suspend") == 0) {
        return suspend_system();
    }

    if (try_loginctl(action) == 0) {
        return 0;
    }

    sync();
    int command = strcmp(action, "poweroff") == 0 ? RB_POWER_OFF : RB_AUTOBOOT;
    if (reboot(command) != 0) {
        perror("rx-greeter-power: reboot");
        return 1;
    }
    return 0;
}
