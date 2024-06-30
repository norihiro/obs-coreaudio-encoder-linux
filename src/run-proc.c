#define _GNU_SOURCE // close_range, pipe2
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include "plugin-macros.generated.h"

pid_t run_proc(const char *proc_path, int *fd_in, int *fd_out, int *fd_err, const char *arg1)
{
	int pipe_in[2] = {-1, -1};
	int pipe_out[2] = {-1, -1};
	int pipe_err[2] = {-1, -1};

	if (fd_in && pipe2(pipe_in, O_CLOEXEC) < 0) {
		blog(LOG_ERROR, "failed to create pipe");
		return -1;
	}

	if (fd_out && pipe2(pipe_out, O_CLOEXEC) < 0) {
		blog(LOG_ERROR, "failed to create pipe");
		goto fail1;
	}

	if (fd_err && pipe2(pipe_err, O_CLOEXEC) < 0) {
		blog(LOG_ERROR, "failed to create pipe");
		goto fail2;
	}

	pid_t pid = fork();
	if (pid < 0) {
		blog(LOG_ERROR, "failed to fork");
		goto fail3;
	}

	if (pid == 0) {
		// I'm a child
		if (pipe_in[0] >= 0) {
			dup2(pipe_in[0], 0);
			close(pipe_in[0]);
			close(pipe_in[1]);
		}
		if (pipe_out[0] >= 0) {
			dup2(pipe_out[1], 1);
			close(pipe_out[0]);
			close(pipe_out[1]);
		}
		if (pipe_err[0] >= 0) {
			dup2(pipe_err[1], 2);
			close(pipe_err[0]);
			close(pipe_err[1]);
		}
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__)
		closefrom(3);
#else // Linux
		close_range(3, 65535, 0);
#endif
#ifdef ENV_WINEPATH
		setenv("WINEPATH", ENV_WINEPATH, 0);
#endif
		setenv("WINEDEBUG", "fixme-all", 0);
		if (execlp(WINE_EXE_PATH, WINE_EXE_PATH, proc_path, arg1, NULL) < 0) {
			fprintf(stderr, "Error: failed to exec \"%s\"\n", proc_path);
			exit(1);
		}
	}

	if (fd_in) {
		*fd_in = pipe_in[1];
		close(pipe_in[0]);
	}
	if (fd_out) {
		*fd_out = pipe_out[0];
		close(pipe_out[1]);
	}
	if (fd_err) {
		*fd_err = pipe_err[0];
		close(pipe_err[1]);
	}

	return pid;

fail3:
	if (pipe_err[0] >= 0) {
		close(pipe_err[0]);
		close(pipe_err[1]);
	}

fail2:
	if (pipe_out[0] >= 0) {
		close(pipe_out[0]);
		close(pipe_out[1]);
	}

fail1:
	if (pipe_in[0] >= 0) {
		close(pipe_in[0]);
		close(pipe_in[1]);
	}

	return -1;
}
