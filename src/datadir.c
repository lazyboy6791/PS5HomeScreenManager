#include "datadir.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"

#define PATH_MAX_LEN 1024

static char g_err[240];

static void fail(const char *what, const char *path) {
  snprintf(g_err, sizeof(g_err), "%s %.150s: %s", what, path, strerror(errno));
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Removes a file or a whole folder. 0 if everything is gone. */
static int rm_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;
  int rc = 0;
  DIR *d = opendir(path);
  if (!d) return -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char sub[PATH_MAX_LEN];
    if ((size_t)snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= sizeof(sub)) {
      rc = -1;
      continue;
    }
    if (rm_tree(sub) != 0) rc = -1;
  }
  closedir(d);
  if (rmdir(path) != 0) rc = -1;
  return rc;
}

/* SHA-256 of a file; -1 if it can't be read. */
static int hash_file(const char *path, uint8_t out[32], unsigned long long *size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  sha256_ctx_t c;
  sha256_init(&c);
  unsigned char buf[65536];
  unsigned long long total = 0;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    sha256_update(&c, buf, (size_t)n);
    total += (unsigned long long)n;
  }
  int bad = n < 0;
  close(fd);
  if (bad) return -1;
  sha256_final(&c, out);
  if (size) *size = total;
  return 0;
}

/* Copies one file and checks the copy by reading it back. */
static int copy_file(const char *src, const char *dst, mode_t mode, unsigned long long *bytes) {
  int in = open(src, O_RDONLY);
  if (in < 0) {
    fail("couldn't read", src);
    return -1;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, (mode & 0777) | 0600);
  if (out < 0) {
    fail("couldn't create", dst);
    close(in);
    return -1;
  }
  sha256_ctx_t c;
  sha256_init(&c);
  unsigned char buf[65536];
  unsigned long long total = 0;
  ssize_t n;
  int bad = 0;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    sha256_update(&c, buf, (size_t)n);
    total += (unsigned long long)n;
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(out, buf + off, (size_t)(n - off));
      if (w <= 0) {
        bad = 1;
        break;
      }
      off += w;
    }
    if (bad) break;
  }
  if (n < 0) bad = 1;
  if (bad) fail("couldn't copy", src);
  if (!bad && fsync(out) != 0) {
    fail("couldn't write", dst);
    bad = 1;
  }
  close(in);
  if (close(out) != 0 && !bad) {
    fail("couldn't write", dst);
    bad = 1;
  }
  if (bad) return -1;
  uint8_t want[32], got[32];
  unsigned long long got_size = 0;
  sha256_final(&c, want);
  if (hash_file(dst, got, &got_size) != 0 || got_size != total || memcmp(want, got, 32) != 0) {
    snprintf(g_err, sizeof(g_err), "the copy of %.150s doesn't match the original", src);
    return -1;
  }
  *bytes += total;
  return 0;
}

static int copy_tree(const char *src, const char *dst, int depth, long *files, unsigned long long *bytes) {
  struct stat st;
  if (stat(src, &st) != 0) {
    fail("couldn't look at", src);
    return -1;
  }
  if (mkdir(dst, (st.st_mode & 0777) | 0700) != 0) {
    fail("couldn't create", dst);
    return -1;
  }
  DIR *d = opendir(src);
  if (!d) {
    fail("couldn't open", src);
    return -1;
  }
  struct dirent *e;
  int rc = 0;
  while (rc == 0 && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    /* the old own copy of the program gets the new name */
    const char *name_out = (depth == 0 && !strcmp(e->d_name, "HomeHider.elf")) ? "HomeScreenManager.elf" : e->d_name;
    if ((size_t)snprintf(from, sizeof(from), "%s/%s", src, e->d_name) >= sizeof(from) ||
        (size_t)snprintf(to, sizeof(to), "%s/%s", dst, name_out) >= sizeof(to)) {
      snprintf(g_err, sizeof(g_err), "a path is too long: %.150s", from);
      rc = -1;
      break;
    }
    struct stat s2;
    if (lstat(from, &s2) != 0) {
      fail("couldn't look at", from);
      rc = -1;
    } else if (S_ISDIR(s2.st_mode)) {
      rc = copy_tree(from, to, depth + 1, files, bytes);
    } else if (S_ISREG(s2.st_mode)) {
      rc = copy_file(from, to, s2.st_mode, bytes);
      if (rc == 0) (*files)++;
    } /* anything else (links, devices) is not part of the data and is left behind */
  }
  closedir(d);
  return rc;
}

int datadir_move(const char *old_dir, const char *new_dir, char *note, size_t note_size) {
  if (note && note_size) note[0] = '\0';
  if (!old_dir || !old_dir[0] || !new_dir || !new_dir[0] || !strcmp(old_dir, new_dir)) return 0;
  if (is_dir(new_dir)) return 0; /* already moved (or a fresh start of the new version) */
  if (!is_dir(old_dir)) return 0; /* nothing to move */

  char tmp[PATH_MAX_LEN];
  if ((size_t)snprintf(tmp, sizeof(tmp), "%s.moving", new_dir) >= sizeof(tmp)) return 0;
  rm_tree(tmp); /* what an interrupted move left behind */

  long files = 0;
  unsigned long long bytes = 0;
  g_err[0] = '\0';
  if (copy_tree(old_dir, tmp, 0, &files, &bytes) != 0) {
    rm_tree(tmp);
    if (note) snprintf(note, note_size, "couldn't move the data folder %s to %s (%s): still using the old folder", old_dir, new_dir, g_err);
    return -1;
  }
  if (rename(tmp, new_dir) != 0) {
    fail("couldn't put the copy in place at", new_dir);
    rm_tree(tmp);
    if (note) snprintf(note, note_size, "couldn't move the data folder %s to %s (%s): still using the old folder", old_dir, new_dir, g_err);
    return -1;
  }
  int removed = rm_tree(old_dir);
  if (note)
    snprintf(note, note_size, "moved the data folder %s to %s (%ld file(s), %llu bytes, every file checked)%s", old_dir,
             new_dir, files, bytes, removed == 0 ? "" : "; the old folder couldn't be removed completely and was left");
  return 1;
}
