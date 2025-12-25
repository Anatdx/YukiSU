#ifndef MANAGER_SIGN_H
#define MANAGER_SIGN_H

// First Manager
#define EXPECTED_SIZE_FIRST 0x16b
#define EXPECTED_HASH_FIRST                                                    \
	"03b53b8bd866c029f2bb34798376977c43874dd589ddaf594080bcef0267a45b"

// Second Manager
#define EXPECTED_SIZE_SECOND 0x1c8
#define EXPECTED_HASH_SECOND                                                   \
	"e76c912ef2def3470f7293b73f983cfc795d7d61c46f85a7013d1fb745deaf89"

typedef struct {
	unsigned size;
	const char *sha256;
} apk_sign_key_t;

#endif /* MANAGER_SIGN_H */
