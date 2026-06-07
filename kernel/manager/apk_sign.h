#ifndef __KSU_H_APK_V2_SIGN
#define __KSU_H_APK_V2_SIGN

#include "ksu.h"
#include <linux/types.h>

struct apk_sign_match {
	int index;
	bool trusted;
	const char *name;
	u32 size;
	char hash[65];
};

bool match_apk_signature(char *path, struct apk_sign_match *match);
bool is_manager_apk(char *path);

/** Same as is_manager_apk; when signature_index is non-NULL, set it to the
 * matched key index (0=first manager, 1=second, etc.). */
bool is_manager_apk_ex(char *path, int *signature_index);

#endif // #ifndef __KSU_H_APK_V2_SIGN
