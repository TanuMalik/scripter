/*
 * Copyright (c) 2017  Zhihao Yuan
 * Copyright (c) 2010, 2012  David E. O'Brien
 * Copyright (c) 1980, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <pty.h>
#include <utmp.h>
#endif

static FILE *fscript;
static int master;
static int child;
static const char *fname;
static int qflg, ttyflg;
static int rawin, showexit;

static struct termios tt;

static _Noreturn void done(int);
static void doshell(char **);
static void finish(void);
static void usage(char *);

int
main(int argc, char *argv[])
{
	ssize_t cc;
	struct termios rtt, stt;
	struct winsize win;
	struct timeval tv, *tvp;
	time_t tvec, start;
	char obuf[BUFSIZ];
	fd_set rfd;
	int aflg, Fflg, ch, k, n;
	int flushtime, readstdin;
	char *argv0 = argv[0];

	aflg = Fflg = 0;
	flushtime = 30;
	showexit = 0;

	while ((ch = getopt(argc, argv, "aFiqt:")) != -1)
		switch (ch) {
		case 'a':
			aflg = 1;
			break;
		case 'F':
			Fflg = 1;
			break;
		case 'i':
			rawin = 1;
			break;
		case 'q':
			qflg = 1;
			break;
		case 't':
			flushtime = atoi(optarg);
			if (flushtime < 0)
				err(1, "invalid flush time %d", flushtime);
			break;
		case '?':
		default:
			usage(argv0);
		}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fname = argv[0];
		argv++;
		argc--;
	} else
		fname = "typescript";

	if ((ttyflg = isatty(STDIN_FILENO)) != 0) {
		if (tcgetattr(STDIN_FILENO, &tt) == -1)
			err(1, "tcgetattr");
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1)
			err(1, "ioctl");
		child = forkpty(&master, NULL, &tt, &win);
	} else {
		child = forkpty(&master, NULL, NULL, NULL);
	}

	if (child < 0)
		err(1, "forkpty");
	if (child == 0)
		doshell(argv);

	if ((fscript = fopen(fname, aflg ? "a" : "w")) == NULL)
		err(1, "%s", fname);

	if (!qflg) {
		tvec = time(NULL);
		(void)printf("Script started, output file is %s\n", fname);
		if (!rawin) {
			(void)fprintf(fscript, "Script started on %s",
			              ctime(&tvec));
			if (argv[0]) {
				showexit = 1;
				fprintf(fscript, "Command: ");
				for (k = 0; argv[k]; ++k)
					fprintf(fscript, "%s%s", k ? " " : "",
					        argv[k]);
				fprintf(fscript, "\n");
			}
		}
		fflush(fscript);
	}
	(void)tcgetattr(master, &stt);
	if (ttyflg) {
		rtt = tt;
		cfmakeraw(&rtt);
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);
	} else if (rawin) {
		cfmakeraw(&stt);
		stt.c_lflag |= ECHO;
		(void)tcsetattr(master, TCSAFLUSH, &stt);
	}

	start = tvec = time(0);
	readstdin = 1;
	for (;;) {
		FD_ZERO(&rfd);
		FD_SET(master, &rfd);
		if (readstdin)
			FD_SET(STDIN_FILENO, &rfd);
		if (!readstdin && ttyflg) {
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			tvp = &tv;
			readstdin = 1;
		} else if (flushtime > 0) {
			tv.tv_sec = flushtime - (tvec - start);
			tv.tv_usec = 0;
			tvp = &tv;
		} else {
			tvp = NULL;
		}
		n = select(master + 1, &rfd, 0, 0, tvp);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && FD_ISSET(STDIN_FILENO, &rfd)) {
			cc = read(STDIN_FILENO, obuf, BUFSIZ);
			if (cc < 0)
				break;
			if (cc == 0) {
				if ((stt.c_lflag & ICANON) != 0)
					(void)write(master, &stt.c_cc[VEOF], 1);
				readstdin = 0;
			}
			if (cc > 0) {
				(void)write(master, obuf, cc);
				if (rawin)
					(void)fwrite(obuf, 1, cc, fscript);
			}
		}
		if (n > 0 && FD_ISSET(master, &rfd)) {
			cc = read(master, obuf, sizeof(obuf));
			if (cc <= 0)
				break;
			(void)write(STDOUT_FILENO, obuf, cc);
			if (!rawin)
				(void)fwrite(obuf, 1, cc, fscript);
		}
		tvec = time(0);
		if (tvec - start >= flushtime) {
			fflush(fscript);
			start = tvec;
		}
		if (Fflg)
			fflush(fscript);
	}
	finish();
	done(0);
}

static void
usage(char *argv0)
{
	(void)fprintf(
	    stderr,
	    "usage: %s [-aiq] [-F pipe] [-t time] [file [command ...]]\n",
	    basename(argv0));
	exit(1);
}

static void
finish(void)
{
	int e, status;

	if (waitpid(child, &status, 0) == child) {
		if (WIFEXITED(status))
			e = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			e = WTERMSIG(status);
		else /* can't happen */
			e = 1;
		done(e);
	}
}

static void
doshell(char **av)
{
	const char *shell;

	shell = getenv("SHELL");
	if (shell == NULL)
		shell = getpwuid(geteuid())->pw_shell;

	setenv("SCRIPT", fname, 1);
	if (av[0]) {
		execvp(av[0], av);
		warn("%s", av[0]);
	} else {
		execl(shell, shell, (char *)NULL);
		warn("%s", shell);
	}
	exit(1);
}

static void
done(int eno)
{
	time_t tvec;

	if (ttyflg)
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);
	tvec = time(NULL);
	if (!qflg) {
		if (!rawin) {
			if (showexit)
				(void)fprintf(fscript,
				              "\nCommand exit status: %d", eno);
			(void)fprintf(fscript, "\nScript done on %s",
			              ctime(&tvec));
		}
		(void)printf("\nScript done, output file is %s\n", fname);
	}
	(void)fclose(fscript);
	(void)close(master);
	exit(eno);
}
