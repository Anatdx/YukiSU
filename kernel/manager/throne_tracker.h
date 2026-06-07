#ifndef __KSU_H_THRONE_TRACKER
#define __KSU_H_THRONE_TRACKER

#ifdef CONFIG_KSU_DISABLE_MANAGER
static inline void ksu_throne_tracker_init(void)
{
}

static inline void ksu_throne_tracker_exit(void)
{
}

static inline void track_throne(bool prune_only)
{
	(void)prune_only;
}

static inline void ksu_request_manager_rescan(void)
{
}
#else
void ksu_throne_tracker_init(void);

void ksu_throne_tracker_exit(void);

void track_throne(bool prune_only);
void ksu_request_manager_rescan(void);
#endif // #ifdef CONFIG_KSU_DISABLE_MANAGER

#endif // #ifndef __KSU_H_THRONE_TRACKER
