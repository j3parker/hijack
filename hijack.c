#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void tty_restore() {
  fflush(STDIN_FILENO);
  tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void fatal(const char* str, ...) {
  int err = errno;

  va_list args;
  va_start(args, str);

  vfprintf(stderr, str, args);
  fprintf(stderr, "\n\terrno = %d\n\terror = %s\n", err, strerror(err));
  va_end(args);
  exit(-1);
}

void register_epoll(int epfd, int fd, int events) {
  struct epoll_event e;
  e.data.fd = fd;
  e.events = events;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e)) {
    fatal("epoll_ctl failed");
  }
}

void write2(int fd, const char* buf, size_t len) {
  while (len > 0) {
    ssize_t count = write(fd, buf, len);
    if (count == -1) {
      fatal("write() failed");
    }
    len -= count;
    buf += count;
  }
}

int handle(struct epoll_event* e, int cmdfd, int in_fd, int out_fd) {
  const size_t kBufSize = 1024;
  char buf[kBufSize];

  if (e->events & ~(EPOLLHUP | EPOLLIN)) {
    fatal("unexepected event %d on fd=%d", e->events, e->data.fd);
  }

  if (e->events & EPOLLIN) {
    ssize_t count = read(e->data.fd, buf, kBufSize);
    if (count == -1) {
      fatal("bad read()");
    }

    if (e->data.fd == STDIN_FILENO) {
      write2(cmdfd, buf, count);
    } else if (e->data.fd == cmdfd) {
      write2(STDOUT_FILENO, buf, count);
      write2(out_fd, buf, count);
    } else if (e->data.fd == in_fd) {
      write2(cmdfd, buf, count);
    } else {
      fatal("unexpected fd for EPOLLIN event");
    }
  }

  if (e->events & EPOLLHUP) {
    if (e->data.fd == cmdfd) {
      exit(0);  // Eh?
    } else if (e->data.fd == in_fd) {
      return 1;
    } else {
      fatal("unexpected");
    }
  }
  return 0;
}

void init_tty() {
  if (tcgetattr(STDIN_FILENO, &orig_termios)) {
    fatal("tcgetattr failed");
  }

  if (atexit(tty_restore)) {
    fatal("couldn't call atexit");
  }

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  // TODO: what of this voodoo is necessary?
  raw.c_cc[VMIN] = 5;
  raw.c_cc[VTIME] = 8;
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  raw.c_cc[VMIN] = 2;
  raw.c_cc[VTIME] = 0;
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 8;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) {
    fatal("couldn't set terminal to raw mode");
  }
}

int init_epoll(int cmdfd) {
  int epfd = epoll_create(2);

  if (epfd == -1) {
    fatal("epoll_create failed");
  }

  register_epoll(epfd, STDIN_FILENO, EPOLLIN);
  register_epoll(epfd, cmdfd, EPOLLIN);
  return epfd;
}

void init_fs(char* dir, char** in_path, int* out_fd) {
  char* path = malloc(strlen(dir) + 5);

  sprintf(path, "%s/out", dir);
  *out_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
  if (*out_fd == -1) {
    fatal("open() for out file failed");
  }

  sprintf(path, "%s/in", dir);
  if (unlink(path) && errno != ENOENT) {
    fatal("unlink()'ing in fifo failed");
  }

  *in_path = path;
}

int open_fifo(char* path, int epfd) {
  if (access(path, F_OK) == -1) {
    if (errno != ENOENT) {
      fatal("access() failed");
    }
    if (mkfifo(path, S_IRWXU)) {
      fatal("mkfifo() failed");
    }
  }
  int fd = open(path, O_RDONLY | O_NONBLOCK, 0);
  if (fd == -1) {
    fatal("open() on fifo failed");
  }

  register_epoll(epfd, fd, EPOLLIN);

  return fd;
}

void bridge(char* dir, int cmdfd) {
  int in_fd, out_fd;
  char* in_path;

  init_fs(dir, &in_path, &out_fd);
  init_tty();
  int epfd = init_epoll(cmdfd);
  in_fd = open_fifo(in_path, epfd);

  const size_t kMaxEvents = 64;
  struct epoll_event events[kMaxEvents];

  while (1) {
    int count = epoll_wait(epfd, events, kMaxEvents, -1);
    if (count == -1) {
      fatal("epoll_wait failed");
    }

    for (int i = 0; i < count; i++) {
      if (handle(&events[i], cmdfd, in_fd, out_fd)) {
        close(in_fd);
        in_fd = open_fifo(in_path, epfd);
      }
    }
  }

  free(in_path);
}

void exec_cmd(int fd, char** argv) {
  setsid();
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (ioctl(fd, TIOCSCTTY, NULL)) {
    fatal("failed to set the controlling terminal for the child process");
  }
  close(fd);
  execvp(argv[0], argv);
  fatal("execvp returned");
}

int main(int argc, char** argv) {
  if (!isatty(STDIN_FILENO)) {
    fprintf(stderr, "You don't want to run this outside a tty...");
    exit(-1);
  }

  if (argc < 3) {
    fprintf(stderr, "Usage: hijack <dir> <cmd ...>\n");
    exit(-1);
  }

  int m, s;

  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

  if (openpty(&m, &s, NULL, NULL, &w)) {
    fatal("openpty failed");
  }

  pid_t pid = fork();

  if (pid == -1) {
    fatal("fork failed");
  }

  if (pid == 0) {
    close(m);
    exec_cmd(s, argv + 2);
  } else {
    close(s);
    bridge(argv[1], m);
  }
}
