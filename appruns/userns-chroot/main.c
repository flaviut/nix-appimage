#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exit status to use when launching an AppImage fails.
 * For applications that assign meanings to exit status codes (e.g. rsync),
 * we avoid "cluttering" pre-defined exit status codes by using 127 which
 * is known to alias an application exit status and also known as launcher
 * error, see SYSTEM(3POSIX).
 */
#define EXIT_EXECERROR 127

static const char *argv0;
static const char *appdir;
static const char *mountroot;
static const size_t max_line_bytes = 1024 * 1024;

static void die_if(bool cond, const char *fmt, ...) {
  if (cond) {
    fprintf(stderr, "%s: ", argv0);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, ": %s\n", strerror(errno));
    exit(EXIT_EXECERROR);
  }
}

char *strprintf(const char *fmt, ...) {
  va_list args1;
  va_start(args1, fmt);
  va_list args2;
  va_copy(args2, args1);

  int len = vsnprintf(NULL, 0, fmt, args1);
  if (len < 0) {
    fprintf(stderr, "%s: vsnprintf '%s' failed\n", argv0, fmt);
    exit(EXIT_EXECERROR);
  }

  char *buf = malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "%s: malloc %d\n", argv0, len + 1);
    exit(EXIT_EXECERROR);
  }

  va_end(args1);

  if (vsnprintf(buf, len + 1, fmt, args2) != len) {
    fprintf(stderr, "%s: vsnprintf '%s' returned unexpected length\n", argv0,
            fmt);
    exit(EXIT_EXECERROR);
  }

  va_end(args2);

  return buf;
}

static int write_to(const char *path, const char *fmt, ...) {
  int fd = open(path, O_WRONLY);
  if (fd > 0) {
    va_list args;
    va_start(args, fmt);
    if (vdprintf(fd, fmt, args) < 0) {
      va_end(args);
      close(fd);
      return 1;
    }
    va_end(args);
    close(fd);
    return 0;
  }
  return 1;
}

struct string_array {
  char **items;
  size_t len;
  size_t cap;
};

static void string_array_free(struct string_array *arr) {
  if (!arr) {
    return;
  }
  for (size_t i = 0; i < arr->len; i++) {
    free(arr->items[i]);
  }
  free(arr->items);
  arr->items = NULL;
  arr->len = 0;
  arr->cap = 0;
}

static int string_array_push(struct string_array *arr, const char *value) {
  if (!value) {
    return -1;
  }
  if (arr->len == arr->cap) {
    size_t new_cap = arr->cap == 0 ? 8 : arr->cap * 2;
    char **new_items = realloc(arr->items, new_cap * sizeof(char *));
    if (!new_items) {
      return -1;
    }
    arr->items = new_items;
    arr->cap = new_cap;
  }
  arr->items[arr->len] = strdup(value);
  if (!arr->items[arr->len]) {
    return -1;
  }
  arr->len++;
  return 0;
}

static bool string_array_contains(const struct string_array *arr,
                                  const char *value) {
  for (size_t i = 0; i < arr->len; i++) {
    if (strcmp(arr->items[i], value) == 0) {
      return true;
    }
  }
  return false;
}

static char *trim_in_place(char *str) {
  while (isspace((unsigned char)str[0])) {
    str++;
  }

  size_t len = strlen(str);
  while (len > 0 && isspace((unsigned char)str[len - 1])) {
    str[--len] = 0;
  }

  return str;
}

struct elf_id {
  int elf_class;
  uint16_t machine;
};

static int read_elf_id(const char *path, struct elf_id *out) {
  if (!out) {
    return -1;
  }

  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }

  unsigned char ident[EI_NIDENT];
  ssize_t n = read(fd, ident, sizeof(ident));
  if (n != (ssize_t)sizeof(ident)) {
    close(fd);
    return -1;
  }
  if (memcmp(ident, ELFMAG, SELFMAG) != 0) {
    close(fd);
    return -1;
  }

  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return -1;
  }

  if (ident[EI_CLASS] == ELFCLASS32) {
    Elf32_Ehdr hdr;
    n = read(fd, &hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) {
      close(fd);
      return -1;
    }
    out->elf_class = ELFCLASS32;
    out->machine = hdr.e_machine;
    close(fd);
    return 0;
  }

  if (ident[EI_CLASS] == ELFCLASS64) {
    Elf64_Ehdr hdr;
    n = read(fd, &hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) {
      close(fd);
      return -1;
    }
    out->elf_class = ELFCLASS64;
    out->machine = hdr.e_machine;
    close(fd);
    return 0;
  }

  close(fd);
  return -1;
}

static bool ld_debug_enabled(void) {
  const char *env = getenv("NIX_APPIMAGE_DEBUG_LD");
  return env && env[0] != 0;
}

static char *read_elf_interp_dir(const char *path) {
	if (!path || path[0] == 0) {
		return NULL;
	}

  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return NULL;
  }

  unsigned char ident[EI_NIDENT];
  ssize_t n = read(fd, ident, sizeof(ident));
  if (n != (ssize_t)sizeof(ident)) {
    close(fd);
    return NULL;
  }
  if (memcmp(ident, ELFMAG, SELFMAG) != 0) {
    close(fd);
    return NULL;
  }

  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return NULL;
  }

	char *result = NULL;
	if (ident[EI_CLASS] == ELFCLASS32) {
		Elf32_Ehdr hdr;
		n = read(fd, &hdr, sizeof(hdr));
		if (n != (ssize_t)sizeof(hdr)) {
			close(fd);
			return NULL;
		}

		for (uint16_t i = 0; i < hdr.e_phnum; i++) {
			Elf32_Phdr phdr;
			off_t off = (off_t)hdr.e_phoff + (off_t)i * (off_t)hdr.e_phentsize;
			if (lseek(fd, off, SEEK_SET) < 0) {
				break;
			}
			n = read(fd, &phdr, sizeof(phdr));
			if (n != (ssize_t)sizeof(phdr)) {
				break;
			}
			if (phdr.p_type != PT_INTERP) {
				continue;
			}

			char *interp = malloc(phdr.p_filesz + 1);
			if (!interp) {
				break;
			}
			if (lseek(fd, (off_t)phdr.p_offset, SEEK_SET) < 0) {
				free(interp);
				break;
			}
			n = read(fd, interp, phdr.p_filesz);
			if (n != (ssize_t)phdr.p_filesz) {
				free(interp);
				break;
			}
			interp[phdr.p_filesz] = 0;

			char *dir_buf = strdup(interp);
			free(interp);
			if (!dir_buf) {
				break;
			}
			char *dir = dirname(dir_buf);
			if (dir) {
				result = strdup(dir);
			}
			free(dir_buf);
			break;
		}
	} else if (ident[EI_CLASS] == ELFCLASS64) {
		Elf64_Ehdr hdr;
		n = read(fd, &hdr, sizeof(hdr));
		if (n != (ssize_t)sizeof(hdr)) {
			close(fd);
			return NULL;
		}

		for (uint16_t i = 0; i < hdr.e_phnum; i++) {
			Elf64_Phdr phdr;
			off_t off = (off_t)hdr.e_phoff + (off_t)i * (off_t)hdr.e_phentsize;
			if (lseek(fd, off, SEEK_SET) < 0) {
				break;
			}
			n = read(fd, &phdr, sizeof(phdr));
			if (n != (ssize_t)sizeof(phdr)) {
				break;
			}
			if (phdr.p_type != PT_INTERP) {
				continue;
			}

			char *interp = malloc(phdr.p_filesz + 1);
			if (!interp) {
				break;
			}
			if (lseek(fd, (off_t)phdr.p_offset, SEEK_SET) < 0) {
				free(interp);
				break;
			}
			n = read(fd, interp, phdr.p_filesz);
			if (n != (ssize_t)phdr.p_filesz) {
				free(interp);
				break;
			}
			interp[phdr.p_filesz] = 0;

			char *dir_buf = strdup(interp);
			free(interp);
			if (!dir_buf) {
				break;
			}
			char *dir = dirname(dir_buf);
			if (dir) {
				result = strdup(dir);
			}
			free(dir_buf);
			break;
		}
	}

	close(fd);
	return result;
}

static char *find_entrypoint_interp_dir(void) {
	char *entrypoint = strprintf("%s/entrypoint", appdir);
	char exe[PATH_MAX + 1];
	ssize_t exe_size = readlink(entrypoint, exe, PATH_MAX);
	if (exe_size < 0) {
		if (ld_debug_enabled()) {
			fprintf(stderr, "%s: entrypoint readlink failed: %s\n", argv0, strerror(errno));
		}
		free(entrypoint);
		return NULL;
	}
	exe[exe_size] = 0;
	if (ld_debug_enabled()) {
		fprintf(stderr, "%s: entrypoint target '%s'\n", argv0, exe);
	}
	free(entrypoint);

	char *interp_dir = read_elf_interp_dir(exe);
	if (!interp_dir && strncmp(exe, "/nix/", 5) == 0) {
		char *bundled = strprintf("%s%s", appdir, exe);
		if (bundled) {
			interp_dir = read_elf_interp_dir(bundled);
			free(bundled);
		}
	}
	if (!interp_dir && ld_debug_enabled()) {
		fprintf(stderr, "%s: entrypoint interp dir not found\n", argv0);
	}
	return interp_dir;
}

static int collect_ldconfig_dirs(struct string_array *collected) {
  struct elf_id self_id = {0};
  if (read_elf_id("/proc/self/exe", &self_id) != 0) {
    return -1;
  }

  const char *cmds[] = {
      "LC_ALL=C ldconfig -p",
      "LC_ALL=C /sbin/ldconfig -p",
      "LC_ALL=C /usr/sbin/ldconfig -p",
      NULL,
  };

  FILE *pipe = NULL;
  for (size_t i = 0; cmds[i]; i++) {
    pipe = popen(cmds[i], "r");
    if (pipe) {
      break;
    }
  }
  if (!pipe) {
    return -1;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, pipe)) != -1) {
    if ((size_t)linelen > max_line_bytes) {
      free(line);
      pclose(pipe);
      return -1;
    }

    char *arrow = strstr(line, "=>");
    if (!arrow) {
      continue;
    }

    char *path = trim_in_place(arrow + 2);
    if (!path || path[0] == 0) {
      continue;
    }

    struct elf_id lib_id = {0};
    if (read_elf_id(path, &lib_id) != 0) {
      if (ld_debug_enabled()) {
        fprintf(stderr, "%s: ldconfig skip non-ELF '%s'\n", argv0, path);
      }
      continue;
    }
    if (lib_id.elf_class != self_id.elf_class ||
        lib_id.machine != self_id.machine) {
      continue;
    }

    char *dir_buf = strdup(path);
    if (!dir_buf) {
      free(line);
      pclose(pipe);
      return -1;
    }
    char *dir = dirname(dir_buf);
    if (dir && !string_array_contains(collected, dir)) {
      if (string_array_push(collected, dir) != 0) {
        free(dir_buf);
        free(line);
        pclose(pipe);
        return -1;
      }
      if (ld_debug_enabled()) {
        fprintf(stderr, "%s: ldconfig add dir '%s'\n", argv0, dir);
      }
    }
    free(dir_buf);
  }

  free(line);
  pclose(pipe);
  return 0;
}

static void extend_ld_library_path(void) {
	struct string_array parsed = {0};
	struct string_array entries = {0};
	char *interp_dir = find_entrypoint_interp_dir();
	if (interp_dir) {
		if (string_array_push(&entries, interp_dir) != 0) {
			free(interp_dir);
			goto cleanup;
		}
		if (ld_debug_enabled()) {
			fprintf(stderr, "%s: entrypoint interp dir '%s'\n", argv0, interp_dir);
		}
		free(interp_dir);
	}
	if (collect_ldconfig_dirs(&parsed) != 0) {
		string_array_free(&parsed);
		return;
	}
  const char *env_ld = getenv("LD_LIBRARY_PATH");
  if (env_ld && env_ld[0] != 0) {
    const char *cursor = env_ld;
    while (true) {
      const char *colon = strchr(cursor, ':');
      size_t len = colon ? (size_t)(colon - cursor) : strlen(cursor);
      if (len > 0) {
        char *segment = strndup(cursor, len);
        if (!segment || string_array_push(&entries, segment) != 0) {
          free(segment);
          goto cleanup;
        }
        free(segment);
      }
      if (!colon) {
        break;
      }
      cursor = colon + 1;
    }
  }

  for (size_t i = 0; i < parsed.len; i++) {
    if (string_array_contains(&entries, parsed.items[i])) {
      continue;
    }
    if (string_array_push(&entries, parsed.items[i]) != 0) {
      goto cleanup;
    }
  }

  size_t total = 0;
  for (size_t i = 0; i < entries.len; i++) {
    total += strlen(entries.items[i]);
    if (i + 1 < entries.len) {
      total++;
    }
  }

  if (total == 0) {
    goto cleanup;
  }

  char *combined = malloc(total + 1);
  if (!combined) {
    goto cleanup;
  }

  size_t offset = 0;
  for (size_t i = 0; i < entries.len; i++) {
    size_t len = strlen(entries.items[i]);
    memcpy(combined + offset, entries.items[i], len);
    offset += len;
    if (i + 1 < entries.len) {
      combined[offset++] = ':';
    }
  }
  combined[offset] = 0;

  if (setenv("LD_LIBRARY_PATH", combined, 1) < 0) {
    fprintf(stderr, "%s: unable to set LD_LIBRARY_PATH: %s\n", argv0,
            strerror(errno));
  } else if (ld_debug_enabled()) {
    fprintf(stderr, "%s: LD_LIBRARY_PATH='%s'\n", argv0, combined);
  }

  free(combined);

cleanup:
  string_array_free(&entries);
  string_array_free(&parsed);
}

void child_main(char **argv) {
  // get uid, gid before going to new namespace
  uid_t uid = getuid();
  gid_t gid = getgid();

  extend_ld_library_path();

  int clonens = CLONE_NEWNS;
  if (uid != 0) {
    // create new user ns so we can mount() in userland
    clonens |= CLONE_NEWUSER;
  }

  // Create new mount namespace (and potentially user namespace if not root)
  die_if(unshare(clonens) < 0, "cannot unshare");

  if (uid != 0) {
    // UID/GID Mapping
    // -----------------------------------------------------------

    // see user_namespaces(7)
    // > The data written to uid_map (gid_map) must consist of a single line
    // that > maps the writing process's effective user ID (group ID) in the
    // parent > user namespace to a user ID (group ID) in the user namespace.
    die_if(write_to("/proc/self/uid_map", "%d %d 1\n", uid, uid),
           "cannot write uid_map");

    // see user_namespaces(7):
    // > In the case of gid_map, use of the setgroups(2) system call must first
    // > be denied by writing "deny" to the /proc/[pid]/setgroups file (see
    // > below) before writing to gid_map.
    die_if(write_to("/proc/self/setgroups", "deny"), "cannot write setgroups");
    die_if(write_to("/proc/self/gid_map", "%d %d 1\n", uid, gid),
           "cannot write gid_map");
  }

  // Mountpoint ----------------------------------------------------------------

  // tmpfs so we don't need to cleanup
  die_if(mount("tmpfs", mountroot, "tmpfs", 0, 0) < 0, "mount tmpfs -> %s",
         mountroot);
  // make unbindable to both prevent event propagation as well as mount
  // explosion
  die_if(mount(mountroot, mountroot, "none", MS_UNBINDABLE, 0) < 0,
         "mount tmpfs bind -> %s", mountroot);

  // copy over root directories
  DIR *rootdir = opendir("/");
  struct dirent *rootentry;
  while ((rootentry = readdir(rootdir))) {
    // ignore . and .. and nix
    if (strcmp(rootentry->d_name, ".") == 0 ||
        strcmp(rootentry->d_name, "..") == 0 ||
        strcmp(rootentry->d_name, "nix") == 0) {
      continue;
    }

    char *from = strprintf("/%s", rootentry->d_name);
    char *to = strprintf("%s/%s", mountroot, rootentry->d_name);

    // we don't treat failure of the below bind as an actual failure, since
    // our logic not robust enough to handle weird filesystem scenarios

    // TODO imitate symlinks as symlinks

    struct stat statbuf;
    if (stat(from, &statbuf) < 0) {
      fprintf(stderr, "%s: stat %s: %s\n", argv0, from, strerror(errno));
    } else {
      if (S_ISDIR(statbuf.st_mode)) {
        die_if(mkdir(to, statbuf.st_mode & ~S_IFMT) < 0, "mkdir %s", to);
        if (mount(from, to, "none", MS_BIND | MS_REC, 0) < 0) {
          fprintf(stderr, "%s: mount %s -> %s: %s\n", argv0, from, to,
                  strerror(errno));
        }
      } else {
        // effectively touch
        int fd = creat(to, statbuf.st_mode & ~S_IFMT);
        if (fd == -1) {
          fprintf(stderr, "%s: creat %s: %s\n", argv0, to, strerror(errno));
        } else {
          close(fd);
          if (mount(from, to, "none", MS_BIND | MS_REC, 0) < 0) {
            fprintf(stderr, "%s: mount %s -> %s: %s\n", argv0, from, to,
                    strerror(errno));
          }
        }
      }
    }

    free(from);
    free(to);
  }

  // mount in /nix
  char *nix_from = strprintf("%s/nix", appdir);
  char *nix_to = strprintf("%s/nix", mountroot);

  die_if(mkdir(nix_to, 0777) < 0, "mkdir %s", nix_to);
  die_if(mount(nix_from, nix_to, "none", MS_BIND | MS_REC, 0) < 0,
         "mount %s -> %s", nix_from, nix_to);

  free(nix_from);
  free(nix_to);

  // Chroot --------------------------------------------------------------------

  // save where we were so we can cd into it
  char cwd[PATH_MAX];
  die_if(!getcwd(cwd, PATH_MAX), "cannot getcwd");

  // chroot
  die_if(chroot(mountroot) < 0, "cannot chroot %s", mountroot);

  // cd back again
  die_if(chdir(cwd) < 0, "cannot chdir %s", cwd);

  // Exec ----------------------------------------------------------------------

  // For better error messages, we wanna get what entrypoint points to
  char *entrypoint = strprintf("%s/entrypoint", appdir);
  char exe[PATH_MAX + 1];
  ssize_t exe_size = readlink(entrypoint, exe, PATH_MAX);
  die_if(exe_size < 0, "cannot read link %s", entrypoint);
  exe[exe_size] = 0;
  free(entrypoint);

  execv(exe, argv);
  die_if(true, "cannot exec %s", exe);
}

int main(int argc, char **argv) {
  argv0 = argv[0];

  // get location of exe
  char appdir_buf[PATH_MAX];
  appdir = dirname(realpath("/proc/self/exe", appdir_buf));
  die_if(!appdir, "cannot access /proc/self/exe");

  // use <appdir>/mountpoint as alternate root. Since this already exists
  // inside the squashfs, we don't need to remove this dir later (which we
  // would have had to do if using mktemp)!
  mountroot = strprintf("%s/mountroot", appdir);

  child_main(argv);
}
