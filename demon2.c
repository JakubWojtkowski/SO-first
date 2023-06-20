#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define DEFAULT_INTERVAL 2

volatile sig_atomic_t signal_received = 0;

void signal_handler(int sig)
{
    signal_received = sig;
}

void search_files(const char *dir_path, const char *pattern, bool verbose)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        if (verbose)
            syslog(LOG_NOTICE, "Ominięcie niedostępnego katalogu: %s", dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        if (signal_received == SIGUSR1)
        {
            closedir(dir);
            return;
        }

        struct stat st;
        if (stat(path, &st) == -1)
        {
            if (verbose)
                syslog(LOG_NOTICE, "Pominięcie niedostępnego pliku: %s", path);
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (access(path, R_OK | X_OK) == 0)
                search_files(path, pattern, verbose);
        }
        else
        {
            if (strstr(entry->d_name, pattern))
            {
                time_t now = time(NULL);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%F %T", localtime(&now));
                syslog(LOG_INFO, "%s | %s | %s", time_str, path, pattern);
            }
        }
    }

    closedir(dir);
}

void search_files_recursive(const char *base_dir, char *const *patterns, int pattern_count, bool verbose)
{
    for (int i = 0; i < pattern_count; i++)
    {
        if (signal_received == SIGUSR1 || signal_received == SIGUSR2)
            break;

        search_files(base_dir, patterns[i], verbose);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s [-v] [-t interval] pattern1 [pattern2 ...]\n", argv[0]);
        return 1;
    }

    bool verbose = false;
    int interval = DEFAULT_INTERVAL;
    int opt;
    while ((opt = getopt(argc, argv, "vt:")) != -1)
    {
        switch (opt)
        {
        case 'v':
            verbose = true;
            break;
        case 't':
            interval = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [-v] [-t interval] pattern1 [pattern2 ...]\n", argv[0]);
            return 1;
        }
    }

    if (optind >= argc)
    {
        fprintf(stderr, "Nie podano wzorca.\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return 1;
    }

    if (pid > 0)
        return 0; // Zakończ proces rodzica

// Zmiana procesu w demona
    umask(0);
    setsid();
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    openlog("file_searcher", LOG_PID, LOG_DAEMON);

    signal(SIGUSR1, signal_handler);
    signal(SIGUSR2, signal_handler);

    int pattern_count = argc - optind;
    char **patterns = argv + optind;
    pid_t supervisor_pid = getpid();

    while (1)
    {
        // Tworzenie procesów potomnych
        for (int i = 0; i < pattern_count; i++)
        {
            pid_t child_pid = fork();
            if (child_pid == 0)
            {
                search_files_recursive("/", &patterns[i], 1, verbose);
                _exit(0);
            }
            else if (child_pid < 0)
            {
                syslog(LOG_ERR, "Błąd podczas tworzenia procesu potomnego");
            }
        }

        bool terminated = false;
        while (!terminated)
        {
            int status;
            pid_t child_pid = wait(&status);
            if (child_pid == -1)
            {
                syslog(LOG_ERR, "Błąd podczas oczekiwania na zakończenie procesu potomnego");
                break;
            }

            if (signal_received == SIGUSR1)
            {
                if (verbose)
                    syslog(LOG_NOTICE, "Otrzymano sygnał SIGUSR1");
                kill(-supervisor_pid, SIGUSR1);
            }
            else if (signal_received == SIGUSR2)
            {
                if (verbose)
                    syslog(LOG_NOTICE, "Otrzymano sygnał SIGUSR2");
                kill(-supervisor_pid, SIGUSR2);
                terminated = true;
            }
            else
            {
                terminated = true;
            }
        }

        if (signal_received != SIGUSR1)
        {
            if (verbose)
                syslog(LOG_NOTICE, "Demon uśpiony");
            sleep(interval);
            if (verbose)
                syslog(LOG_NOTICE, "Demon obudzony");
        }
    }

    closelog();

    return 0;
    }
    
