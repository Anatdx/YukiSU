/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel <-> zygiskd netlink ABI.
 *
 * Author: Anatdx
 */
#ifndef _UAPI_YUKIZYGISK_H
#define _UAPI_YUKIZYGISK_H

#include <linux/types.h>

#define YZ_NETLINK_PROTO 27  /* custom netlink protocol number */
#define YZ_NL_GROUP_EVENTS 1 /* multicast group: lifecycle events */
#define YZ_NL_MSG_EVENT 0x10 /* nlmsg_type for a yz_event */

enum yz_event_type {
  YZ_EV_SPECIALIZE = 1,
};

struct yz_event {
  __u32 type; /* enum yz_event_type */
  __u32 pid;
  __u32 appid;
};

#endif /* _UAPI_YUKIZYGISK_H */
