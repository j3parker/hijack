#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int res;
#define ERR_FATAL(cmd)                                                         \
  {                                                                            \
    res = cmd;                                                                 \
    if (res == -1) {                                                           \
      fprintf(stderr, "dying at " __FILE__ ":%d\n", __LINE__);                 \
      fprintf(stderr, "\tres=%d, errno=%d\n", res, errno);                     \
      exit(-1);                                                                \
    }                                                                          \
  }

#define RETRY(cmd)                                                             \
  while ((res = cmd) == -1) {                                                  \
    if (errno == EINTR) {                                                      \
      continue;                                                                \
    }                                                                          \
    printf("dying at " __FILE__ ":%d\n", __LINE__);                            \
    exit(-1);                                                                  \
  }

int stdin_pipe[2];
int stdout_pipe[2];
int stderr_pipe[2];
int fs_stdin_fd;
int fs_stdout_fd;
int fs_stderr_fd;

#define FD_OUT 0
#define FD_IN 1

char file[1024];
void open_infile() {
  ERR_FATAL(open(file, O_RDONLY | O_NONBLOCK));
  fs_stdin_fd = res;
}

void handle(struct epoll_event *e) {
  const size_t kBufSize = 1024;
  static char buf[kBufSize];
  if (e->events & EPOLLHUP && e->data.fd == fs_stdin_fd) {
    ERR_FATAL(close(fs_stdin_fd));
    open_infile();
    return;
  }
  if (!(e->events & EPOLLIN)) {
    printf("unexpected event=%d on fd=%d\n", e->events, e->data.fd);
    exit(-1);
  }

  ERR_FATAL(read(e->data.fd, buf, kBufSize));
  int count = res;

  int wfd, fsfd = -1;
  if (e->data.fd == stdout_pipe[FD_OUT]) {
    wfd = STDOUT_FILENO;
    fsfd = fs_stdout_fd;
  } else if (e->data.fd == stderr_pipe[FD_OUT]) {
    wfd = STDERR_FILENO;
    fsfd = fs_stderr_fd;
  } else if (e->data.fd == fs_stdin_fd) {
    wfd = stdin_pipe[FD_IN];
  } else if (e->data.fd == STDIN_FILENO) {
    wfd = stdin_pipe[FD_IN];
  } else {
    assert(0);
  }

  ERR_FATAL(write(wfd, buf, count));
  assert(count == res);
  if (fsfd != -1) {
    ERR_FATAL(write(fsfd, buf, count));
    assert(count == res);
  }
}

void register_epoll(int epfd, int fd, int events) {
  struct epoll_event e;
  e.data.fd = fd;
  e.events = events;
  ERR_FATAL(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e));
}

void bridge(const char *dir) {
  // Setup fifo and files
  snprintf(file, sizeof(file), "%s/in", dir);
  res = unlink(file);
  if (res != 0 && !(res == -1 && errno == ENOENT)) {
    ERR_FATAL(res);
  }
  ERR_FATAL(mkfifo(file, S_IRWXU));
  open_infile();

  snprintf(file, sizeof(file), "%s/out", dir);
  ERR_FATAL(open(file, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU));
  fs_stdout_fd = res;

  snprintf(file, sizeof(file), "%s/err", dir);
  ERR_FATAL(open(file, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU));
  fs_stderr_fd = res;

  // Setup epoll
  const size_t kMaxEvents = 64;
  struct epoll_event *events = malloc(sizeof(struct epoll_event) * kMaxEvents);
  assert(events);

  ERR_FATAL(epoll_create(4));
  int epfd = res;

  ERR_FATAL(close(stdin_pipe[FD_OUT]));
  ERR_FATAL(close(stdout_pipe[FD_IN]));
  ERR_FATAL(close(stderr_pipe[FD_IN]));

  register_epoll(epfd, stdout_pipe[FD_OUT], EPOLLIN);
  register_epoll(epfd, stderr_pipe[FD_OUT], EPOLLIN);
  register_epoll(epfd, STDIN_FILENO, EPOLLIN);
  register_epoll(epfd, fs_stdin_fd, EPOLLIN);
  // TODO:  EPOLLOUT | EPOLLET on outputs? buffers?

  while (1) {
    RETRY(epoll_wait(epfd, events, kMaxEvents, -1));
    int count = res;

    for (int i = 0; i < count; i++) {
      handle(&events[i]);
    }
  }
}

void client(char **argv) {
  RETRY(dup2(stdin_pipe[FD_OUT], STDIN_FILENO));
  RETRY(dup2(stdout_pipe[FD_IN], STDOUT_FILENO));
  RETRY(dup2(stderr_pipe[FD_IN], STDERR_FILENO));

  ERR_FATAL(close(stdin_pipe[FD_IN]));
  ERR_FATAL(close(stdin_pipe[FD_OUT]));
  ERR_FATAL(close(stdout_pipe[FD_IN]));
  ERR_FATAL(close(stdout_pipe[FD_OUT]));
  ERR_FATAL(close(stderr_pipe[FD_IN]));
  ERR_FATAL(close(stderr_pipe[FD_OUT]));

  ERR_FATAL(execvp(argv[0], argv));
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: wrap <path> <cmd...>\n\tpath: directory to put "
                    "stdout, stderr and stdin files\n\tcmd: command to wrap\n");
    exit(-1);
  }
  ERR_FATAL(pipe(stdin_pipe));
  ERR_FATAL(pipe(stdout_pipe));
  ERR_FATAL(pipe(stderr_pipe));

  pid_t pid = 0;
  ERR_FATAL(pid = fork());

  if (pid) {
    bridge(argv[1]);
  } else {
    client(argv + 2);
  }
}
