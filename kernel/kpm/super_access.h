#ifndef __YUKISU_SUPER_ACCESS_H
#define __YUKISU_SUPER_ACCESS_H

#include "compact.h"
#include "kpm.h"
#include <linux/stddef.h>
#include <linux/types.h>

extern int yukisu_super_find_struct(const char *struct_name, size_t *out_size,
				    int *out_members);
extern int yukisu_super_access(const char *struct_name, const char *member_name,
			       size_t *out_offset, size_t *out_size);
extern int yukisu_super_container_of(const char *struct_name,
				     const char *member_name, void *ptr,
				     void **out_ptr);

#endif // #ifndef __YUKISU_SUPER_ACCESS_H
