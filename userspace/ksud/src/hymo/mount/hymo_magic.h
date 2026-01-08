#ifndef _LINUX_HYMO_MAGIC_H
#define _LINUX_HYMO_MAGIC_H

#include <stddef.h>

#define HYMO_MAGIC1 0x48594D4F  // "HYMO"
#define HYMO_MAGIC2 0x524F4F54  // "ROOT"
#define HYMO_PROTOCOL_VERSION 10

// HymoFS command codes (used via KSU ioctl)
#define HYMO_CMD_ADD_RULE 0x48001
#define HYMO_CMD_DEL_RULE 0x48002
#define HYMO_CMD_HIDE_RULE 0x48003
#define HYMO_CMD_INJECT_RULE 0x48004
#define HYMO_CMD_CLEAR_ALL 0x48005
#define HYMO_CMD_GET_VERSION 0x48006
#define HYMO_CMD_LIST_RULES 0x48007
#define HYMO_CMD_SET_DEBUG 0x48008
#define HYMO_CMD_REORDER_MNT_ID 0x48009
#define HYMO_CMD_SET_STEALTH 0x48010
#define HYMO_CMD_HIDE_OVERLAY_XATTRS 0x48011
#define HYMO_CMD_ADD_MERGE_RULE 0x48012
#define HYMO_CMD_SET_AVC_LOG_SPOOFING 0x48013
#define HYMO_CMD_SET_MIRROR_PATH 0x48014

struct hymo_syscall_arg {
    const char* src;
    const char* target;
    int type;
};

struct hymo_syscall_list_arg {
    char* buf;
    size_t size;
};

#endif // #ifndef _LINUX_HYMO_MAGIC_H
