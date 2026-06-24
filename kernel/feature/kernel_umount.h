#ifndef __KSU_H_KERNEL_UMOUNT
#define __KSU_H_KERNEL_UMOUNT

#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/types.h>

void ksu_kernel_umount_init(void);
void ksu_kernel_umount_exit(void);

void try_umount(const char *mnt, int flags);

// Schedule a mount-revert on an arbitrary app task (YukiZygisk YZ_UMOUNT_PID).
struct task_struct;
int ksu_umount_task_modules(struct task_struct *task);

// Handler function to be called from setresuid hook
int ksu_handle_umount(uid_t old_uid, uid_t new_uid);

// for the umount list
struct mount_entry {
	char *umountable;
	unsigned int flags;
	struct list_head list;
};
extern struct list_head mount_list;
extern struct rw_semaphore mount_list_lock;

#endif // #ifndef __KSU_H_KERNEL_UMOUNT
