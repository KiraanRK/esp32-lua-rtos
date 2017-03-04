#include "esp_attr.h"

#include <limits.h>
#include <reent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/mount.h>

extern int __real_mkdir(const char* name, mode_t mode);

int __wrap_mkdir(const char* name, mode_t mode) {
	char *ppath;
	int res;

	ppath = mount_resolve_to_physical(name);
	res = __real_mkdir(ppath, mode);

	free(ppath);

	return res;
}
