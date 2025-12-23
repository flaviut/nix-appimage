#define _GNU_SOURCE
#include <dirent.h>
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
#include <ctype.h>

/* Exit status to use when launching an AppImage fails.
 * For applications that assign meanings to exit status codes (e.g. rsync),
 * we avoid "cluttering" pre-defined exit status codes by using 127 which
 * is known to alias an application exit status and also known as launcher
 * error, see SYSTEM(3POSIX).
 */
#define EXIT_EXECERROR 127

static const char* argv0;
static const char* appdir;
static const char* mountroot;
static const size_t max_line_bytes = 1024 * 1024;

static void die_if(bool cond, const char* fmt, ...)
{
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

char* strprintf(const char* fmt, ...)
{
	va_list args1;
	va_start(args1, fmt);
	va_list args2;
	va_copy(args2, args1);

	int len = vsnprintf(NULL, 0, fmt, args1);
	if (len < 0) {
		fprintf(stderr, "%s: vsnprintf '%s' failed\n", argv0, fmt);
		exit(EXIT_EXECERROR);
	}

	char* buf = malloc(len + 1);
	if (!buf) {
		fprintf(stderr, "%s: malloc %d\n", argv0, len + 1);
		exit(EXIT_EXECERROR);
	}

	va_end(args1);

	if (vsnprintf(buf, len + 1, fmt, args2) != len) {
		fprintf(stderr, "%s: vsnprintf '%s' returned unexpected length\n", argv0, fmt);
		exit(EXIT_EXECERROR);
	}

	va_end(args2);

	return buf;
}

static int write_to(const char* path, const char* fmt, ...)
{
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
	char** items;
	size_t len;
	size_t cap;
};

static void string_array_free(struct string_array* arr)
{
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

static int string_array_push(struct string_array* arr, const char* value)
{
	if (!value) {
		return -1;
	}
	if (arr->len == arr->cap) {
		size_t new_cap = arr->cap == 0 ? 8 : arr->cap * 2;
		char** new_items = realloc(arr->items, new_cap * sizeof(char*));
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

static bool string_array_contains(const struct string_array* arr, const char* value)
{
	for (size_t i = 0; i < arr->len; i++) {
		if (strcmp(arr->items[i], value) == 0) {
			return true;
		}
	}
	return false;
}

static char* trim_in_place(char* str)
{
	while (isspace((unsigned char) str[0])) {
		str++;
	}

	size_t len = strlen(str);
	while (len > 0 && isspace((unsigned char) str[len - 1])) {
		str[--len] = 0;
	}

	return str;
}

static bool matches_star(const char* name, const char* pattern)
{
	const char* star = strchr(pattern, '*');
	if (!star) {
		return strcmp(name, pattern) == 0;
	}

	size_t prefix_len = (size_t) (star - pattern);
	size_t suffix_len = strlen(star + 1);
	size_t name_len = strlen(name);

	return name_len >= prefix_len + suffix_len
		&& strncmp(name, pattern, prefix_len) == 0
		&& strcmp(name + name_len - suffix_len, star + 1) == 0;
}

static int cmp_strings(const void* a, const void* b)
{
	const char* const* lhs = a;
	const char* const* rhs = b;
	return strcmp(*lhs, *rhs);
}

static char* canonicalize_or_copy(const char* path)
{
	char* resolved = realpath(path, NULL);
	if (resolved) {
		return resolved;
	}
	return strdup(path);
}

static int parse_conf(const char* path, struct string_array* seen, struct string_array* collected);

static int expand_include(const char* pattern, struct string_array* seen, struct string_array* collected)
{
	if (strchr(pattern, '*')) {
		char* parent_buf = strdup(pattern);
		char* basename_buf = strdup(pattern);
		if (!parent_buf || !basename_buf) {
			free(parent_buf);
			free(basename_buf);
			return -1;
		}

		char* parent_dir = dirname(parent_buf);
		if (!parent_dir) {
			parent_dir = ".";
		}
		char* pattern_name = basename(basename_buf);

		DIR* dir = opendir(parent_dir);
		if (!dir) {
			free(parent_buf);
			free(basename_buf);
			return -1;
		}

		struct string_array files = { 0 };
		struct dirent* entry;
		while ((entry = readdir(dir))) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}

			char* joined = strprintf("%s/%s", parent_dir, entry->d_name);
			if (!joined) {
				string_array_free(&files);
				closedir(dir);
				free(parent_buf);
				free(basename_buf);
				return -1;
			}

			bool is_regular = entry->d_type == DT_REG;
			if (entry->d_type == DT_UNKNOWN) {
				struct stat st;
				is_regular = stat(joined, &st) == 0 && S_ISREG(st.st_mode);
			}

			if (!is_regular || !matches_star(entry->d_name, pattern_name)) {
				free(joined);
				continue;
			}

			if (string_array_push(&files, joined) != 0) {
				free(joined);
				string_array_free(&files);
				closedir(dir);
				free(parent_buf);
				free(basename_buf);
				return -1;
			}

			free(joined);
		}
		closedir(dir);

		qsort(files.items, files.len, sizeof(char*), cmp_strings);

		for (size_t i = 0; i < files.len; i++) {
			int rc = parse_conf(files.items[i], seen, collected);
			if (rc != 0) {
				string_array_free(&files);
				free(parent_buf);
				free(basename_buf);
				return rc;
			}
		}

		string_array_free(&files);
		free(parent_buf);
		free(basename_buf);
		return 0;
	}

	return parse_conf(pattern, seen, collected);
}

static int parse_conf(const char* path, struct string_array* seen, struct string_array* collected)
{
	char* canonical = canonicalize_or_copy(path);
	if (!canonical) {
		return -1;
	}

	if (string_array_contains(seen, canonical)) {
		free(canonical);
		return 0;
	}

	char* parent_buf = strdup(canonical);
	if (!parent_buf) {
		free(canonical);
		return -1;
	}
	char* parent_dir = dirname(parent_buf);
	if (!parent_dir) {
		parent_dir = ".";
	}

	if (string_array_push(seen, canonical) != 0) {
		free(canonical);
		free(parent_buf);
		return -1;
	}

	FILE* file = fopen(canonical, "r");
	free(canonical);
	if (!file) {
		free(parent_buf);
		return -1;
	}

	char* line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, file)) != -1) {
		if ((size_t) linelen > max_line_bytes) {
			free(line);
			fclose(file);
			free(parent_buf);
			return -1;
		}

		char* hash = strchr(line, '#');
		if (hash) {
			*hash = 0;
		}

		char* trimmed = trim_in_place(line);
		if (trimmed[0] == 0) {
			continue;
		}

		if (strncmp(trimmed, "include", 7) == 0) {
			char* rest = trim_in_place(trimmed + 7);
			if (rest[0] == 0) {
				continue;
			}

			char* include_path;
			if (rest[0] == '/') {
				include_path = strdup(rest);
			} else {
				include_path = strprintf("%s/%s", parent_dir, rest);
			}

			if (!include_path) {
				free(line);
				fclose(file);
				free(parent_buf);
				return -1;
			}

			int rc = expand_include(include_path, seen, collected);
			free(include_path);
			if (rc != 0) {
				free(line);
				fclose(file);
				free(parent_buf);
				return rc;
			}
		} else {
			if (string_array_push(collected, trimmed) != 0) {
				free(line);
				fclose(file);
				free(parent_buf);
				return -1;
			}
		}
	}

	free(line);
	fclose(file);
	free(parent_buf);
	return 0;
}

static int parse_ld_so_conf(const char* path, struct string_array* parsed)
{
	struct string_array seen = { 0 };

	int rc = parse_conf(path, &seen, parsed);
	if (rc != 0) {
		string_array_free(parsed);
	}

	string_array_free(&seen);
	return rc;
}

static void extend_ld_library_path(void)
{
	struct string_array parsed = { 0 };
	if (parse_ld_so_conf("/etc/ld.so.conf", &parsed) != 0) {
		string_array_free(&parsed);
		return;
	}

	struct string_array entries = { 0 };
	const char* env_ld = getenv("LD_LIBRARY_PATH");
	if (env_ld && env_ld[0] != 0) {
		const char* cursor = env_ld;
		while (true) {
			const char* colon = strchr(cursor, ':');
			size_t len = colon ? (size_t) (colon - cursor) : strlen(cursor);
			if (len > 0) {
				char* segment = strndup(cursor, len);
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

	char* combined = malloc(total + 1);
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
		fprintf(stderr, "%s: unable to set LD_LIBRARY_PATH: %s\n", argv0, strerror(errno));
	}

	free(combined);

cleanup:
	string_array_free(&entries);
	string_array_free(&parsed);
}

void child_main(char** argv)
{
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
		// UID/GID Mapping -----------------------------------------------------------

		// see user_namespaces(7)
		// > The data written to uid_map (gid_map) must consist of a single line that
		// > maps the writing process's effective user ID (group ID) in the parent
		// > user namespace to a user ID (group ID) in the user namespace.
		die_if(write_to("/proc/self/uid_map", "%d %d 1\n", uid, uid), "cannot write uid_map");

		// see user_namespaces(7):
		// > In the case of gid_map, use of the setgroups(2) system call must first
		// > be denied by writing "deny" to the /proc/[pid]/setgroups file (see
		// > below) before writing to gid_map.
		die_if(write_to("/proc/self/setgroups", "deny"), "cannot write setgroups");
		die_if(write_to("/proc/self/gid_map", "%d %d 1\n", uid, gid), "cannot write gid_map");
	}

	// Mountpoint ----------------------------------------------------------------

	// tmpfs so we don't need to cleanup
	die_if(mount("tmpfs", mountroot, "tmpfs", 0, 0) < 0, "mount tmpfs -> %s", mountroot);
	// make unbindable to both prevent event propagation as well as mount explosion
	die_if(mount(mountroot, mountroot, "none", MS_UNBINDABLE, 0) < 0, "mount tmpfs bind -> %s", mountroot);

	// copy over root directories
	DIR* rootdir = opendir("/");
	struct dirent* rootentry;
	while ((rootentry = readdir(rootdir))) {
		// ignore . and .. and nix
		if (strcmp(rootentry->d_name, ".") == 0
			|| strcmp(rootentry->d_name, "..") == 0
			|| strcmp(rootentry->d_name, "nix") == 0) {
			continue;
		}

		char* from = strprintf("/%s", rootentry->d_name);
		char* to = strprintf("%s/%s", mountroot, rootentry->d_name);

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
					fprintf(stderr, "%s: mount %s -> %s: %s\n", argv0, from, to, strerror(errno));
				}
			} else {
				// effectively touch
				int fd = creat(to, statbuf.st_mode & ~S_IFMT);
				if (fd == -1) {
					fprintf(stderr, "%s: creat %s: %s\n", argv0, to, strerror(errno));
				} else {
					close(fd);
					if (mount(from, to, "none", MS_BIND | MS_REC, 0) < 0) {
						fprintf(stderr, "%s: mount %s -> %s: %s\n", argv0, from, to, strerror(errno));
					}
				}
			}
		}

		free(from);
		free(to);
	}

	// mount in /nix
	char* nix_from = strprintf("%s/nix", appdir);
	char* nix_to = strprintf("%s/nix", mountroot);

	die_if(mkdir(nix_to, 0777) < 0, "mkdir %s", nix_to);
	die_if(mount(nix_from, nix_to, "none", MS_BIND | MS_REC, 0) < 0, "mount %s -> %s", nix_from, nix_to);

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
	char* entrypoint = strprintf("%s/entrypoint", appdir);
	char exe[PATH_MAX + 1];
	ssize_t exe_size = readlink(entrypoint, exe, PATH_MAX);
	die_if(exe_size < 0, "cannot read link %s", entrypoint);
	exe[exe_size] = 0;
	free(entrypoint);

	execv(exe, argv);
	die_if(true, "cannot exec %s", exe);
}

int main(int argc, char** argv)
{
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
