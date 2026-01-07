#ifndef KSU_FILE_WRAPPER_H
#define KSU_FILE_WRAPPER_H

#include <linux/file.h>
#include <linux/fs.h>

#ifdef CONFIG_KSU_LKM
int ksu_install_file_wrapper(int fd);
void ksu_file_wrapper_init(void);
#else
struct ksu_file_wrapper {
	struct file *orig;
	struct file_operations ops;
};

struct ksu_file_wrapper *ksu_create_file_wrapper(struct file *fp);
void ksu_delete_file_wrapper(struct ksu_file_wrapper *data);
#endif // #ifdef CONFIG_KSU_LKM
#endif // #ifndef KSU_FILE_WRAPPER_H
