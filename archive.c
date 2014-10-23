/* Copyright 2011-2013 Jari Vetoniemi
 *
 * This file is part of sxiv.
 *
 * sxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * sxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#include <archive.h>
#include <archive_entry.h>

#include "image.h"
#include "thumbs.h"
#include "util.h"

extern char * ACHV_EXTRCT;

static int archive_pathcmp(const void *a, const void *b) { return strcoll(*(char**)a, *(char**)b); }

/* stupid rar files, libarchive doesn't have enough support for these yet
 * below are stupid unrar wrappers */

static bool unrar(const char *archive, const char *file, const char *dst)
{
	int ret = 1;
	char *bn, *copy;
	pid_t pid;

	if (!archive || !file || !dst)
		return false;

	if ((pid = fork()) == -1)
		return false;

	/* child */
	if (pid == 0) {
		if ((copy = s_strdup((char*)dst)) == NULL)
			return false;

		if ((bn = strrchr(copy, '/')) != NULL && bn[1] != '\0') {
			*bn = 0;
			r_mkdir(copy);
			execlp("unrar", "unrar", "e", "-y", "-inul", archive, file, copy, NULL);
		}
		free(copy);
		exit(EXIT_FAILURE);
	}

	waitpid(pid, &ret, 0);
	return ret == 0;
}

static bool unrar_is_dir(const char *archive, const char *file)
{
	pid_t pid;
	int fd[2], newlines = 0;

	if (!archive || pipe(fd) == -1)
		return false;

	if ((pid = fork()) == -1) {
		close(fd[0]);
		close(fd[1]);
		return false;
	}

	/* child */
	if (pid == 0) {
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		execlp("unrar", "unrar", "lb", archive, file, NULL);
		exit(EXIT_FAILURE);
	}

	close(fd[1]);
	waitpid(pid, NULL, 0);

    char *n, buffer[1024];
	memset(buffer, 0, sizeof(buffer));
	while (read(fd[0], buffer, sizeof(buffer)) > 0) {
		for (n = buffer; (n = memchr(n, '\n', sizeof(buffer)-(n-buffer)));) {
			if (++newlines > 1) break;
			++n;
		}
	}
	return (newlines > 1);
}

static char** unrar_ls(const char *archive, int *cnt)
{
	pid_t pid;
	int fd[2];
	char **paths = NULL;
	*cnt = 0;

	if (!archive || pipe(fd) == -1)
		return NULL;

	if ((pid = fork()) == -1) {
		close(fd[0]);
		close(fd[1]);
		return NULL;
	}

	/* child */
	if (pid == 0) {
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		execlp("unrar", "unrar", "lb", archive, NULL);
		exit(EXIT_FAILURE);
	}

	close(fd[1]);
	waitpid(pid, NULL, 0);

	int mbuf = 1024;
	char *buffer, *pos;
	buffer = pos = s_malloc(mbuf);
	memset(buffer, 0, mbuf);

	while (read(fd[0], pos, mbuf-(pos-buffer)) >= mbuf-(pos-buffer)) {
		buffer = s_realloc(buffer, mbuf * 2);
		pos = buffer + mbuf;
		memset(pos, 0, mbuf);
		mbuf *= 2;
	}

	if (strlen(buffer) > 0) {
		int i, l, lstart, memcnt = 10;

		paths = s_malloc(memcnt * sizeof(char*));
		for (lstart = 0, l = 0, i = 0; i < mbuf; ++i) {
			if (buffer[i] == '\n') {
				buffer[i] = '\0';
				if (!unrar_is_dir(archive, buffer+lstart))
					paths[l++] = s_strdup(buffer+lstart);
				lstart = i + 1;
				if (l >= memcnt) {
					memcnt *= 2;
					paths = s_realloc(paths, memcnt * sizeof(char*));
				}
			}
		}
		*cnt = l;
	}

	free(buffer);
	return paths;
}

static char** archive_ls(struct archive *a, int *cnt)
{
	char **paths;
	int ret, i, memcnt = 10;
	struct archive_entry *entry;

	paths = s_malloc(memcnt * sizeof(char*));
	for (i = 0; (ret = archive_read_next_header(a, &entry)) == ARCHIVE_OK;) {
		if (archive_entry_size(entry) <= 0) continue;
		paths[i++] = s_strdup((char*)archive_entry_pathname(entry));
        if (i >= memcnt) {
			memcnt *= 2;
			paths = s_realloc(paths, memcnt * sizeof(char*));
		}
	}

	*cnt = i;
	if (ret == ARCHIVE_FATAL || ret == ARCHIVE_FAILED) {
		for (i = 0; i < *cnt; ++i) free(paths[i]);
		free(paths);
		paths = NULL;
		*cnt = 0;
	}
	return paths;
}

static int archive_copy_data(struct archive *ar, struct archive *aw)
{
	int r;
	const void *buff;
	size_t size;
	off_t offset;

	for (;;) {
		r = archive_read_data_block(ar, &buff, &size, &offset);
		if (r == ARCHIVE_EOF)
			return (ARCHIVE_OK);
		if (r != ARCHIVE_OK)
			return (r);
		r = archive_write_data_block(aw, buff, size, offset);
		if (r != ARCHIVE_OK) {
			warn("%s\n", archive_error_string(aw));
			return (r);
		}
	}

	return ARCHIVE_FAILED;
}

static bool archive_load(const fileinfo_t *file)
{
	char *bn;
	struct archive *a, *ext;
	struct archive_entry *entry;
	int ret = ARCHIVE_FAILED;

    if (file->archive == NULL)
		return false;

	if ((a = archive_read_new()) == NULL)
		return false;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	if (archive_read_open_filename(a, file->archive, 10240) != ARCHIVE_OK) {
		archive_read_free(a);
		return false;
	}

	if (ret != ARCHIVE_OK) {
		if ((ext = archive_write_disk_new()) == NULL) {
			archive_read_free(a);
			return false;
		}

		archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_NO_OVERWRITE);
		while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
			if (archive_entry_size(entry) <= 0) continue;
			if ((bn = strrchr(archive_entry_pathname(entry), '/')) != NULL && bn[1] != '\0')
				bn++;
			else
				bn = (char*)archive_entry_pathname(entry);
			if (!STREQ(bn, file->base)) continue;
			if (archive_format(a) == ARCHIVE_FORMAT_RAR) {
				if (unrar(file->archive, archive_entry_pathname(entry), file->path)) {
					ret = ARCHIVE_OK;
					break;
				}
			}
			archive_entry_set_pathname(entry, file->path);
			archive_write_header(ext, entry);
			ret = archive_copy_data(a, ext);
			archive_write_finish_entry(ext);
			if (ret != ARCHIVE_OK) {
				printf("libarchive: %s\n", archive_error_string(a));
				if (archive_format(a) == ARCHIVE_FORMAT_RAR) printf("try installing unrar for rar archives\n");
			}
			break;
		}

		archive_write_close(ext);
		archive_write_free(ext);
	}

	if (ret != ARCHIVE_OK && archive_format(a) == ARCHIVE_FORMAT_RAR) {
		if (unrar(file->archive, file->path + strlen(ACHV_EXTRCT) + 1, file->path))
			ret = ARCHIVE_OK;
	}

	archive_read_close(a);
	archive_read_free(a);
	return (ret == ARCHIVE_OK);

}

static void archive_remove(const char *path)
{
	char *bn, *copy;
	remove(path);
	if ((copy = s_strdup((char*)path)) != NULL) {
		while ((bn = strrchr(copy, '/')) != NULL && bn[1] != '\0') {
			*bn = 0;
			if (STREQ(copy, ACHV_EXTRCT)) break;
			rmdir(copy);
		}
		free(copy);
	}
	rmdir(ACHV_EXTRCT);
}

bool archive_img_load(img_t *img, const fileinfo_t *file)
{
	bool ret = false;
	if (archive_load(file)) ret = img_load(img, file);
	archive_remove(file->path);
	return ret;
}

bool archive_tns_load(tns_t *tns, int n, const fileinfo_t *file, bool force, bool silent)
{
	bool ret = false;
	if (archive_load(file)) ret = tns_load(tns, n, file, force, silent);
	archive_remove(file->path);
	return ret;
}

bool archive_check_add_file(const char *filename, void (*callback)(char*, char*))
{
	int i, cnt = 0;
	char **paths = NULL;
	struct archive *a;

	if (ACHV_EXTRCT == NULL || filename == NULL)
		return false;

	if ((a = archive_read_new()) == NULL)
		return false;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	if (archive_read_open_filename(a, filename, 10240) != ARCHIVE_OK) {
		archive_read_free(a);
		return false;
	}

	if (!paths)
		paths = archive_ls(a, &cnt);

	if (!paths && archive_format(a) == ARCHIVE_FORMAT_RAR)
		paths = unrar_ls(filename, &cnt);

	if (paths) {
		qsort(paths, cnt, sizeof(char*), archive_pathcmp);
		for (i = 0; i < cnt; ++i) {
			callback(paths[i], (char*)filename);
			free(paths[i]);
		}
		free(paths);
	}

	archive_read_close(a);
	archive_read_free(a);
	return true;
}

bool archive_is_archive(const char *filename)
{
	struct archive *a;
    bool is_archive = true;

	if (ACHV_EXTRCT == NULL || filename == NULL)
		return false;

	if ((a = archive_read_new()) == NULL)
		return false;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	if (archive_read_open_filename(a, filename, 10240) != ARCHIVE_OK)
		is_archive = false;

	archive_read_close(a);
	archive_read_free(a);
	return is_archive;
}

