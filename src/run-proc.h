#pragma once

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t run_proc(const char *proc_path, int *fd_in, int *fd_out, int *fd_err, const char *arg1);

#ifdef __cplusplus
}
#endif
