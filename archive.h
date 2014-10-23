#ifndef ARCHIVE_H
#define ARCHIVE_H

bool archive_img_load(void*, const fileinfo_t*);
bool archive_tns_load(void*, int, const fileinfo_t*, bool, bool);
bool archive_check_add_file(const char*, void (*callback)(char*, char*));
bool archive_is_archive(const char *filename);

#endif /* ARCHIVE_H */
