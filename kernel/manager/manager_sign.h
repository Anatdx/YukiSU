#ifndef MANAGER_SIGN_H
#define MANAGER_SIGN_H

// Release Manager
#define EXPECTED_SIZE 0x29c
#define EXPECTED_HASH                                                          \
	"39559b380d4c0191eed81b7eba61533b6a2f247bc55bceba4259e983673f58b7"

typedef struct {
	u32 size;
	const char *sha256;
} apk_sign_key_t;

#endif /* MANAGER_SIGN_H */
