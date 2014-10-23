/* Copyright 2011-2013 Bert Muennich
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
#define _MAPPINGS_CONFIG

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <X11/keysym.h>
#include <zlib.h>

#include "types.h"
#include "commands.h"
#include "image.h"
#include "archive.h"
#include "options.h"
#include "thumbs.h"
#include "util.h"
#include "window.h"
#include "config.h"

enum {
	FILENAME_CNT = 1024,
	TITLE_LEN    = 256
};

typedef struct {
	struct timeval when;
	bool active;
	timeout_f handler;
} timeout_t;

/* timeout handler functions: */
void redraw(void);
void reset_cursor(void);
void animate(void);
void clear_resize(void);
void load_image(int);

appmode_t mode;
img_t img;
tns_t tns;
win_t win;

/* directory handle for recursive-fast mode */
r_dir_t *dirr = NULL;

fileinfo_t *files;
int memfilecnt, filecnt, fileidx, startidx = 0;
int filearchivecnt = 0;
int markcnt;
int alternate;

int prefix;

bool resized = false;

bool dual_mode = false;
bool manga_mode = false;

const char * const ACHV_EXTRCT = "/tmp/sxiv-unarchive";
const char * const INFO_SCRIPT = ".sxiv/exec/image-info";
struct {
  char *script;
  int fd;
  unsigned int i, lastsep;
  bool open;
} info;

timeout_t timeouts[] = {
	{ { 0, 0 }, false, redraw },
	{ { 0, 0 }, false, reset_cursor },
	{ { 0, 0 }, false, animate },
	{ { 0, 0 }, false, clear_resize },
};

bool session_store(const char *file)
{
    int i;
	FILE *f;

	f = fopen(file, "w");
	if (f == NULL) {
		warn("failed to open session file: %s\n", file);
		return false;
	}

	fprintf(f, "sxiv-session 1.0\n");
	fprintf(f, "%d\n", filecnt);
	fprintf(f, "%d\n", fileidx);
	fprintf(f, "%d\n", dual_mode);
	fprintf(f, "%d\n", manga_mode);
	for (i = 0; i < filecnt; ++i) {
		fprintf(f, "%s\n", (files[i].archive?files[i].archive:""));
		fprintf(f, "%s\n", (files[i].name?files[i].name:""));
		fprintf(f, "%s\n", (files[i].path?files[i].path:""));
		fprintf(f, "%d\n", files[i].linked);
		fprintf(f, "%d\n", files[i].single_page);
		fprintf(f, "%d\n", files[i].marked);
	}

	fclose(f);
	return true;
}

bool session_restore(const char *file)
{
    FILE *f;
	int i, fcnt;
	size_t len, n;
	char *line = NULL;
	char *version = NULL;
	const char *bn;

	f = fopen(file, "r");
	if (f == NULL) {
		if (filecnt == 0)
			warn("failed to open session file: %s\n", file);
		return false;
	}

	len = get_line(&line, &n, f); line[len-1] = '\0';
	if (strncmp(line, "sxiv-session ", strlen("sxiv-session "))) {
		fclose(f);
		warn("session file in wrong format\n");
		return false;
	}

	if ((version = s_strdup(line+strlen("sxiv-session "))) == NULL) {
		fclose(f);
		warn("failed to retieve version from session file");
		return false;
	}

	len = get_line(&line, &n, f); line[len-1] = '\0';
	if ((fcnt = strtol(line, NULL, 10)) <= 0) {
		free(version);
		fclose(f);
		if (filecnt == 0)
			warn("no files in session file\n");
		return false;
	}

	filecnt += fcnt;
	files = (fileinfo_t*) s_malloc(filecnt * sizeof(fileinfo_t));
	memfilecnt = filecnt;
	fileidx = fcnt;

	len = get_line(&line, &n, f); line[len-1] = '\0';
	startidx = strtol(line, NULL, 10);

    if (startidx >= fcnt)
		startidx = fcnt - 1;
	else if (startidx < 0)
		startidx = 0;

	len = get_line(&line, &n, f); line[len-1] = '\0';
	dual_mode = strtol(line, NULL, 10);

	len = get_line(&line, &n, f); line[len-1] = '\0';
	manga_mode = strtol(line, NULL, 10);

	for (i = 0; i != fcnt; ++i) {
		files[i].loaded = false;
		files[i].name = files[i].path = files[i].archive = NULL;

		len = get_line(&line, &n, f); line[len-1] = '\0';
		if (line[0] != '\0') files[i].archive = s_strdup(line);
		len = get_line(&line, &n, f); line[len-1] = '\0';
		if (line[0] != '\0') files[i].name = s_strdup(line);
		len = get_line(&line, &n, f); line[len-1] = '\0';
		if (line[0] != '\0') files[i].path = s_strdup(line);

		len = get_line(&line, &n, f); line[len-1] = '\0';
		files[i].linked = strtol(line, NULL, 10);
		len = get_line(&line, &n, f); line[len-1] = '\0';
		files[i].single_page = strtol(line, NULL, 10);
		len = get_line(&line, &n, f); line[len-1] = '\0';
		files[i].marked = strtol(line, NULL, 10);

		if (files[i].archive && (bn = strrchr(files[i].archive , '/')) != NULL && bn[1] != '\0')
			files[i].archivebase = ++bn;
		else
			files[i].archivebase = files[i].archive;
		if ((bn = strrchr(files[i].name , '/')) != NULL && bn[1] != '\0')
			files[i].base = ++bn;
		else
			files[i].base = files[i].name;
	}

	fclose(f);
	free(version);
	free(line);
	return (filecnt > 0);
}

void cleanup(void)
{
	static bool in = false;

    if (ACHV_EXTRCT)
		rmdir(ACHV_EXTRCT);

	if (dirr)
		r_closedir(dirr);

	if (!in) {
		in = true;
		img_close(&img, false);
		tns_free(&tns);
		win_close(&win);
	}
}

void refresh_archive_count(void)
{
	int i;

	for (i = 0, filearchivecnt = 0; i < filecnt; ++i) {
		if (i + 1 < filecnt && files[i].archive && !s_strcmp(files[i].archive, files[i+1].archive)) continue;
		++filearchivecnt;
	}
}

void refresh_dual_pages(int reset_from)
{
	int i;

	for (i = reset_from; i >= 0 && i < filecnt; ++i)
		files[i].linked = -1;

	for (i = 0; dual_mode && i < filecnt; ++i) {
		if (i + 1 >= filecnt) continue;
		if (files[i].single_page) continue;
		if (files[i].linked != -1) continue;
		if (s_strcmp(files[i].archive, files[i+1].archive)) continue;
		files[i].linked = i+1;
		files[i+1].linked = i;
	}
}

void check_add_file(char *filename, char *archive)
{
	int i;
	char *pathcpy, *bn;
	static const char *achv_ext[] = { ".zip", ".rar", ".7z", ".tar", ".tar.gz", "tar.xz", ".tgz", NULL };

	if (filename == NULL || *filename == '\0')
		return;

	if (archive == NULL && access(filename, R_OK) < 0) {
		warn("could not open file: %s", filename);
		return;
	}

	if (archive != NULL) {
		if (memfilecnt < ++filecnt) {
			while (memfilecnt < filecnt) memfilecnt *= 2;
			files = (fileinfo_t*) s_realloc(files, memfilecnt * sizeof(fileinfo_t));
		}
		for (i = filecnt-1; i > fileidx; --i)
			memcpy(&files[i], &files[i-1], sizeof(fileinfo_t));
	}

	if (fileidx == memfilecnt) {
		memfilecnt *= 2;
		files = (fileinfo_t*) s_realloc(files, memfilecnt * sizeof(fileinfo_t));
	}
	if (*filename != '/' && archive == NULL) {
		files[fileidx].path = absolute_path(filename);
		if (files[fileidx].path == NULL) {
			warn("could not get absolute path of file: %s\n", filename);
			return;
		}
	} else if (archive != NULL) {
		files[fileidx].path = path_append(ACHV_EXTRCT, filename);
	}

	files[fileidx].linked = -1;
	files[fileidx].single_page = false;
	files[fileidx].loaded = false;
	files[fileidx].marked = false;
	files[fileidx].name = s_strdup(filename);

	if (*filename == '/' && archive == NULL)
		files[fileidx].path = files[fileidx].name;

    for (i = 0; !archive && achv_ext[i]; ++i) {
		if (s_strucmp(files[fileidx].path+strlen(files[fileidx].path)-strlen(achv_ext[i]), achv_ext[i])) continue;
		archive = (char*)files[fileidx].path;
	}

	if (archive) {
		files[fileidx].archive = s_strdup(archive);
	} else {
		pathcpy = s_strdup((char*)files[fileidx].path);
		if ((bn = strrchr(pathcpy, '/')) != NULL && bn[1] != '\0')
			*bn = '\0';
		files[fileidx].archive = s_strdup(pathcpy);
		free(pathcpy);
	}

	if (archive && (bn = strrchr(files[fileidx].archive , '/')) != NULL && bn[1] != '\0')
		files[fileidx].archivebase = ++bn;
	else
		files[fileidx].archivebase = files[fileidx].archive;
	if ((bn = strrchr(files[fileidx].name , '/')) != NULL && bn[1] != '\0')
		files[fileidx].base = ++bn;
	else
		files[fileidx].base = files[fileidx].name;
	fileidx++;
}

void remove_file(int n, bool manual)
{
	int ofileidx = fileidx, linked = -1;

	if (n < 0 || n >= filecnt)
		return;

	if (!manual) {
		fileidx = n+1;
		archive_check_add_file(files[n].path, check_add_file);
		if (fileidx > filecnt) filecnt = fileidx;
		fileidx = ofileidx;
	}

	if (filecnt == 1) {
		if (!manual)
			fprintf(stderr, "sxiv: no more files to display, aborting\n");
		cleanup();
		exit(manual ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if (files[n].linked != -1) {
		linked = files[n].linked;
		if (linked > n) --linked;
		files[n].linked = -1;
	}

	if (files[n].path != files[n].name)
		free((void*) files[n].path);
	if (files[n].archive)
		free((void*) files[n].archive);
	free((void*) files[n].name);

	if (n + 1 < filecnt)
		memmove(files + n, files + n + 1, (filecnt - n - 1) * sizeof(fileinfo_t));
	if (n + 1 < tns.cnt) {
		memmove(tns.thumbs + n, tns.thumbs + n + 1, (tns.cnt - n - 1) *
		        sizeof(thumb_t));
		memset(tns.thumbs + tns.cnt - 1, 0, sizeof(thumb_t));
	}

	filecnt--;
	if (n < tns.cnt)
		tns.cnt--;

	refresh_dual_pages(n);
	refresh_archive_count();

	if (dual_mode && linked != -1) {
		files[linked].linked = -1;
		remove_file(linked, manual);
	}
}

void set_timeout(timeout_f handler, int time, bool overwrite)
{
	int i;

	for (i = 0; i < ARRLEN(timeouts); i++) {
		if (timeouts[i].handler == handler) {
			if (!timeouts[i].active || overwrite) {
				gettimeofday(&timeouts[i].when, 0);
				TV_ADD_MSEC(&timeouts[i].when, time);
				timeouts[i].active = true;
			}
			return;
		}
	}
}

void reset_timeout(timeout_f handler)
{
	int i;

	for (i = 0; i < ARRLEN(timeouts); i++) {
		if (timeouts[i].handler == handler) {
			timeouts[i].active = false;
			return;
		}
	}
}

bool check_timeouts(struct timeval *t)
{
	int i = 0, tdiff, tmin = -1;
	struct timeval now;

	while (i < ARRLEN(timeouts)) {
		if (timeouts[i].active) {
			gettimeofday(&now, 0);
			tdiff = TV_DIFF(&timeouts[i].when, &now);
			if (tdiff <= 0) {
				timeouts[i].active = false;
				if (timeouts[i].handler != NULL)
					timeouts[i].handler();
				i = tmin = -1;
			} else if (tmin < 0 || tdiff < tmin) {
				tmin = tdiff;
			}
		}
		i++;
	}
	if (tmin > 0 && t != NULL)
		TV_SET_MSEC(t, tmin);
	return tmin > 0;
}

void open_info(void)
{
	static pid_t pid;
	int pfd[2];

	if (info.script == NULL || info.open || win.bar.h == 0)
		return;
	if (info.fd != -1) {
		close(info.fd);
		kill(pid, SIGTERM);
		info.fd = -1;
	}
	win.bar.l[0] = '\0';

	warn("info script not supported on manga branch yet.");
	return;

	if (pipe(pfd) < 0)
		return;
	pid = fork();
	if (pid > 0) {
		close(pfd[1]);
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
		info.fd = pfd[0];
		info.i = info.lastsep = 0;
		info.open = true;
	} else if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], 1);
		execl(info.script, info.script, files[fileidx].name, NULL);
		warn("could not exec: %s", info.script);
		exit(EXIT_FAILURE);
	}
}

void read_info(void)
{
	ssize_t i, n;
	char buf[BAR_L_LEN];

	while (true) {
		n = read(info.fd, buf, sizeof(buf));
		if (n < 0 && errno == EAGAIN)
			return;
		else if (n == 0)
			goto end;
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				if (info.lastsep == 0) {
					win.bar.l[info.i++] = ' ';
					info.lastsep = 1;
				}
			} else {
				win.bar.l[info.i++] = buf[i];
				info.lastsep = 0;
			}
			if (info.i + 1 == sizeof(win.bar.l))
				goto end;
		}
	}
end:
	info.i -= info.lastsep;
	win.bar.l[info.i] = '\0';
	win_update_bar(&win);
	info.fd = -1;
	while (waitpid(-1, NULL, WNOHANG) > 0);
}

void load_image(int new)
{
	int linked;
    img_t combined, second;

	if (new < 0 || new >= filecnt)
		return;

	win_set_cursor(&win, CURSOR_WATCH);

	img_close(&img, false);
	while (!img_load(&img, &files[new])) {
		remove_file(new, false);
		if (new >= filecnt)
			new = filecnt - 1;
	}

	linked = files[new].linked;
	if (dual_mode && linked != -1) {
		img_init(&second, &win);
		memcpy(&combined, &img, sizeof(img_t));
		if (img_load(&second, &files[linked]) &&
			img_join(&combined, &img, &second, ((manga_mode && linked > new) || (!manga_mode && linked < new)))) {
			img_close(&img, false);
			memcpy(&img, &combined, sizeof(img_t));
		} else {
			refresh_dual_pages((linked > new ? linked : new));
			files[new].linked = -1;
			files[new].single_page = true;
		}
		img_close(&second, false);
	}

	files[new].loaded = true;
	alternate = fileidx;
	fileidx = new;

	info.open = false;
	open_info();

	if (img.multi.cnt > 0 && img.multi.animate)
		set_timeout(animate, img.multi.frames[img.multi.sel].delay, true);
	else
		reset_timeout(animate);

	if (options->session)
		session_store(options->session);
}

void update_info(void)
{
	int sel, pages = 0, page = 1, page2 = -1, fnum, fnum2 = -1, pw = 0, i, fn, fw, n = 0;
	unsigned int llen = sizeof(win.bar.l), rlen = sizeof(win.bar.r);
	char *lt = win.bar.l, *rt = win.bar.r, title[TITLE_LEN];
	const char * mark, *file, *fileb;
	bool ow_info;
	DIR *dir = NULL;

	for (fw = 0, i = filearchivecnt; i > 0; fw++, i /= 10);
	sel = mode == MODE_IMAGE ? fileidx : tns.sel;

	if (!files[sel].archive || (dir = opendir(files[sel].archive))) {
		file = files[sel].name;
		fileb = files[sel].base;
		if (dir) closedir(dir);
	} else {
		file = files[sel].archive;
		fileb = files[sel].archivebase;
	}
	if (!fileb) fileb = file;

	/* count pages of archive */
	if (files[sel].archive != NULL) {
		for (i = sel-1; i >= 0 && !s_strcmp(files[sel].archive, files[i].archive); --i);
		for (++i; i < filecnt && !s_strcmp(files[sel].archive, files[i].archive); ++pages, ++i)
			if (i == sel) page = pages;
		for (pw = 0, i = pages; i > 0; pw++, i /= 10);

		/* figure out dual page numbers */
		if (dual_mode) {
			if (files[sel].linked != -1 && files[sel].linked > sel) page2 = page + 1;
			if (files[sel].linked != -1 && files[sel].linked < sel) page2 = page, --page;
		}
	}

	/* count the pseudo file number (archives are considered single file) */
	for (i = 0, fnum = 0; i < sel; ++i) {
		if (i + 1 < filecnt && files[i].archive && !s_strcmp(files[i].archive, files[i+1].archive)) continue;
		++fnum;
	}

	/* figure out dual file numbers */
	if (dual_mode && files[sel].archive == NULL) {
		if (files[sel].linked != -1 && files[sel].linked > sel) fnum2 = fnum + 1;
		if (files[sel].linked != -1 && files[sel].linked < sel) fnum2 = fnum, --fnum;
	}

	/* update window title */
	if (mode == MODE_THUMB) {
		win_set_title(&win, "sxiv");
	} else {
		snprintf(title, sizeof(title), "sxiv - %s", file);
		win_set_title(&win, title);
	}

	/* update bar contents */
	if (win.bar.h == 0)
		return;

	if (dual_mode || manga_mode)
		n += snprintf(rt + n, rlen - n, "%s%s| ", dual_mode ? "D " : "", manga_mode ? "M " : "");

	mark = files[sel].marked ? "* " : "";
	if (mode == MODE_THUMB) {
		if (tns.cnt == filecnt) {
			if (pages > 0) {
				if (page2 != -1)
					n += snprintf(rt + n, rlen - n, "%0*d,%0*d/%d | ", pw, page + 1, pw, page2 + 1, pages);
				else
					n += snprintf(rt + n, rlen - n, "%0*d/%d | ", pw, page + 1, pages);
			}

			if (fnum2 != -1)
				n += snprintf(rt + n, rlen - n, "%s%0*d,%0*d/%d", mark, fw, fnum + 1, fw, fnum + 2, filearchivecnt);
			else
				n += snprintf(rt + n, rlen - n, "%s%0*d/%d", mark, fw, fnum + 1, filearchivecnt);
			ow_info = true;
		} else {
			snprintf(lt, llen, "Loading... %0*d/%d", fw, tns.cnt, filecnt);
			rt[0] = '\0';
			ow_info = false;
		}
	} else {
		n += snprintf(rt + n, rlen - n, "%s%3d%% | ", mark, (int) (img.zoom * 100.0));
		if (img.multi.cnt > 0) {
			for (fn = 0, i = img.multi.cnt; i > 0; fn++, i /= 10);
			n += snprintf(rt + n, rlen - n, "%0*d/%d | ",
			              fn, img.multi.sel + 1, img.multi.cnt);
		}

		if (pages > 1) {
			if (page2 != -1)
				n += snprintf(rt + n, rlen - n, "%0*d,%0*d/%d | ", pw, page + 1, pw, page2 + 1, pages);
			else
				n += snprintf(rt + n, rlen - n, "%0*d/%d | ", pw, page + 1, pages);
		}

		if (fnum2 != -1)
			n += snprintf(rt + n, rlen - n, "%0*d,%0*d/%d", fw, fnum + 1, fw, fnum + 2, filearchivecnt);
		else
			n += snprintf(rt + n, rlen - n, "%0*d/%d", fw, fnum + 1, filearchivecnt);
		ow_info = info.script == NULL;
	}
	if (ow_info && file) {
		fn = strlen(file);
		if (fn < llen &&
		    win_textwidth(file, fn, true) +
		    win_textwidth(rt, n, true) < win.w)
		{
			strncpy(lt, file, llen);
		} else {
			strncpy(lt, fileb, llen);
		}
	}
}

void redraw(void)
{
	if (mode == MODE_IMAGE)
		img_render(&img);
	else
		tns_render(&tns);
	update_info();
	win_draw(&win);
	reset_timeout(redraw);
	reset_cursor();
}

void reset_cursor(void)
{
	int i;
	cursor_t cursor = CURSOR_NONE;

	if (mode == MODE_IMAGE) {
		for (i = 0; i < ARRLEN(timeouts); i++) {
			if (timeouts[i].handler == reset_cursor) {
				if (timeouts[i].active)
					cursor = CURSOR_ARROW;
				break;
			}
		}
	} else {
		if (tns.cnt != filecnt)
			cursor = CURSOR_WATCH;
		else
			cursor = CURSOR_ARROW;
	}
	win_set_cursor(&win, cursor);
}

void animate(void)
{
	if (img_frame_animate(&img, false)) {
		redraw();
		set_timeout(animate, img.multi.frames[img.multi.sel].delay, true);
	}
}

void clear_resize(void)
{
	resized = false;
}

bool keymask(const keymap_t *k, unsigned int state)
{
	return (k->ctrl ? ControlMask : 0) == (state & ControlMask);
}

bool buttonmask(const button_t *b, unsigned int state)
{
	return ((b->ctrl ? ControlMask : 0) | (b->shift ? ShiftMask : 0)) ==
	       (state & (ControlMask | ShiftMask));
}

void on_keypress(XKeyEvent *kev)
{
	int i;
	KeySym ksym;
	char key;

	if (kev == NULL)
		return;

	XLookupString(kev, &key, 1, &ksym, NULL);

	if ((ksym == XK_Escape || (key >= '0' && key <= '9')) &&
	    (kev->state & ControlMask) == 0)
	{
		/* number prefix for commands */
		prefix = ksym == XK_Escape ? 0 : prefix * 10 + (int) (key - '0');
		return;
	}

	for (i = 0; i < ARRLEN(keys); i++) {
		if (keys[i].ksym == ksym && keymask(&keys[i], kev->state)) {
			if (keys[i].cmd != NULL && keys[i].cmd(keys[i].arg))
				redraw();
			prefix = 0;
			return;
		}
	}
}

void on_buttonpress(XButtonEvent *bev)
{
	int i, sel;

	if (bev == NULL)
		return;

	if (mode == MODE_IMAGE) {
		win_set_cursor(&win, CURSOR_ARROW);
		set_timeout(reset_cursor, TO_CURSOR_HIDE, true);

		for (i = 0; i < ARRLEN(buttons); i++) {
			if (buttons[i].button == bev->button &&
			    buttonmask(&buttons[i], bev->state))
			{
				if (buttons[i].cmd != NULL && buttons[i].cmd(buttons[i].arg))
					redraw();
				return;
			}
		}
	} else {
		/* thumbnail mode (hard-coded) */
		switch (bev->button) {
			case Button1:
				if ((sel = tns_translate(&tns, bev->x, bev->y)) >= 0) {
					if (sel == tns.sel) {
						mode = MODE_IMAGE;
						set_timeout(reset_cursor, TO_CURSOR_HIDE, true);
						load_image(tns.sel);
					} else {
						tns_highlight(&tns, tns.sel, false);
						tns_highlight(&tns, sel, true);
						tns.sel = sel;
					}
					redraw();
					break;
				}
				break;
			case Button4:
			case Button5:
				if (tns_scroll(&tns, bev->button == Button4 ? DIR_UP : DIR_DOWN,
				               (bev->state & ControlMask) != 0))
					redraw();
				break;
		}
	}
}

void run(void)
{
	int xfd, ofileidx;
	fd_set fds;
	struct timeval timeout;
	bool discard, to_set;
	XEvent ev, nextev;
	char *filename;

	redraw();

	while (true) {
		while (mode == MODE_THUMB && tns.cnt < filecnt &&
		       XPending(win.env.dpy) == 0)
		{
			/* load thumbnails */
			set_timeout(redraw, TO_REDRAW_THUMBS, false);
			if (tns_load(&tns, tns.cnt, &files[tns.cnt], false, false)) {
				tns.cnt++;
			} else {
				remove_file(tns.cnt, false);
				if (tns.sel >= tns.cnt)
					tns.sel--;
			}
			if (tns.cnt == filecnt)
				redraw();
			else
				check_timeouts(NULL);
		}

		if (dirr && XPending(win.env.dpy) == 0)
		{
			/* load images (recursive-fast) */
			set_timeout(redraw, TO_LOAD_NEXT, false);
			if ((filename = r_readdir(dirr)) != NULL) {
				ofileidx = fileidx;
				fileidx = filecnt;
				if (archive_is_archive(filename) || img_test(filename))
					check_add_file(filename, NULL);
				  free((void*) filename);
				filecnt = fileidx;
				refresh_dual_pages(fileidx);
				refresh_archive_count();
				fileidx = ofileidx;
				if (mode == MODE_THUMB && filecnt > tns.cap) {
					tns.thumbs = (thumb_t*) s_realloc(tns.thumbs, filecnt*2 * sizeof(thumb_t));
					memset(&tns.thumbs[tns.cap], 0, (filecnt*2-tns.cap) * sizeof(thumb_t));
					tns.cap = filecnt*2;
				}
			} else {
				r_closedir(dirr);
				dirr = NULL;
			}
		}

		while (XPending(win.env.dpy) == 0
		       && ((to_set = check_timeouts(&timeout)) || info.fd != -1))
		{
			/* check for timeouts & input */
			xfd = ConnectionNumber(win.env.dpy);
			FD_ZERO(&fds);
			FD_SET(xfd, &fds);
			if (info.fd != -1) {
				FD_SET(info.fd, &fds);
				xfd = MAX(xfd, info.fd);
			}
			select(xfd + 1, &fds, 0, 0, to_set ? &timeout : NULL);
			if (info.fd != -1 && FD_ISSET(info.fd, &fds))
				read_info();
		}

		do {
			XNextEvent(win.env.dpy, &ev);
			discard = false;
			if (XEventsQueued(win.env.dpy, QueuedAlready) > 0) {
				XPeekEvent(win.env.dpy, &nextev);
				switch (ev.type) {
					case ConfigureNotify:
						discard = ev.type == nextev.type;
						break;
					case KeyPress:
						discard = (nextev.type == KeyPress || nextev.type == KeyRelease)
						          && ev.xkey.keycode == nextev.xkey.keycode;
						break;
				}
			}
		} while (discard);

		switch (ev.type) {
			/* handle events */
			case ButtonPress:
				on_buttonpress(&ev.xbutton);
				break;
			case ClientMessage:
				if ((Atom) ev.xclient.data.l[0] == wm_delete_win)
					return;
				break;
			case ConfigureNotify:
				if (win_configure(&win, &ev.xconfigure)) {
					if (mode == MODE_IMAGE) {
						img.dirty = true;
						img.checkpan = true;
					} else {
						tns.dirty = true;
					}
					if (!resized || win.fullscreen) {
						redraw();
						set_timeout(clear_resize, TO_REDRAW_RESIZE, false);
						resized = true;
					} else {
						set_timeout(redraw, TO_REDRAW_RESIZE, false);
					}
				}
				break;
			case Expose:
				win_expose(&win, &ev.xexpose);
				break;
			case KeyPress:
				on_keypress(&ev.xkey);
				break;
			case MotionNotify:
				if (mode == MODE_IMAGE) {
					win_set_cursor(&win, CURSOR_ARROW);
					set_timeout(reset_cursor, TO_CURSOR_HIDE, true);
				}
				break;
		}
	}
}

int fncmp(const void *a, const void *b)
{
	return strcoll(((fileinfo_t*) a)->name, ((fileinfo_t*) b)->name);
}

int main(int argc, char **argv)
{
	int i, start;
	size_t n;
	ssize_t len;
	char *filename;
	const char *homedir;
	struct stat fstats;
	r_dir_t dir;
	bool restored = false;

	parse_options(argc, argv);

	if (options->clean_cache) {
		tns_init(&tns, 0, NULL);
		tns_clean_cache(&tns);
		exit(EXIT_SUCCESS);
	}

	if (options->filecnt == 0 && !options->from_stdin && !options->session) {
		print_usage();
		exit(EXIT_FAILURE);
	}

	if (options->recursive || options->recursive_fast || options->from_stdin)
		filecnt = FILENAME_CNT;
	else
		filecnt = options->filecnt;

	if (options->session) {
		if (!(restored = session_restore(options->session)) && filecnt == 0)
			exit(EXIT_FAILURE);
	}

	if (!restored) {
		files = (fileinfo_t*) s_malloc(filecnt * sizeof(fileinfo_t));
		memfilecnt = filecnt;
		fileidx = 0;
	}

	if (options->from_stdin) {
		filename = NULL;
		while ((len = get_line(&filename, &n, stdin)) > 0) {
			if (filename[len-1] == '\n')
				filename[len-1] = '\0';
			check_add_file(filename, NULL);
		}
		if (filename != NULL)
			free(filename);
	}

	for (i = 0; i < options->filecnt; i++) {
		filename = options->filenames[i];

		if (stat(filename, &fstats) < 0) {
			warn("could not stat file: %s", filename);
			continue;
		}
		if (!S_ISDIR(fstats.st_mode)) {
			check_add_file(filename, NULL);
		} else {
			if (!options->recursive && !options->recursive_fast) {
				warn("ignoring directory: %s", filename);
				continue;
			}
			if (r_opendir(&dir, filename) < 0) {
				warn("could not open directory: %s", filename);
				continue;
			}
			start = fileidx;

			if (options->recursive_fast) {
				dirr = &dir;
				while ((filename = r_readdir(&dir)) != NULL) {
					if (archive_is_archive(filename) || img_test(filename))
						check_add_file(filename, NULL);
					free((void*) filename);
					if ((!dual_mode && fileidx > 0) || fileidx > 1)
						break;
				}
				if (filename == NULL) {
					r_closedir(&dir);
					dirr = NULL;
				}
			} else {
				while ((filename = r_readdir(&dir)) != NULL) {
					check_add_file(filename, NULL);
					free((void*) filename);
				}
				r_closedir(&dir);
				if (fileidx - start > 1)
					qsort(files + start, fileidx - start, sizeof(fileinfo_t), fncmp);
			}
		}
	}

	if (fileidx == 0 && !options->session) {
		fprintf(stderr, "sxiv: no valid image file given, aborting\n");
		exit(EXIT_FAILURE);
	}

	filecnt = fileidx;
	fileidx = options->startnum < filecnt ? options->startnum : 0;
	if (startidx != 0 && startidx != options->startnum) fileidx = startidx;
	refresh_dual_pages(-1);
	refresh_archive_count();

	win_init(&win);
	img_init(&img, &win);

	if ((homedir = getenv("HOME")) == NULL) {
		warn("could not locate home directory");
	} else {
		len = strlen(homedir) + strlen(INFO_SCRIPT) + 2;
		info.script = (char*) s_malloc(len);
		snprintf(info.script, len, "%s/%s", homedir, INFO_SCRIPT);
		if (access(info.script, X_OK) != 0) {
			free(info.script);
			info.script = NULL;
		}
	}
	info.fd = -1;

	if (options->thumb_mode) {
		mode = MODE_THUMB;
		tns_init(&tns, filecnt, &win);
		while (!tns_load(&tns, 0, &files[0], false, false))
			remove_file(0, false);
		tns.cnt = 1;
	} else {
		mode = MODE_IMAGE;
		tns.thumbs = NULL;
		load_image(fileidx);
	}

	win_open(&win);

	run();
	cleanup();

	return 0;
}
