#ifndef CINNABAR_FAST_IMPORT_H
#define CINNABAR_FAST_IMPORT_H

extern int maybe_handle_command(const char *command, struct string_list *args);

extern void *get_object_entry(const unsigned char *sha1);

#endif
