#ifndef MANAGER_SIGN_H
#define MANAGER_SIGN_H

// YukiSU Manager
#define EXPECTED_SIZE_FIRST 0x29c
#define EXPECTED_HASH_FIRST                                                    \
	"39559b380d4c0191eed81b7eba61533b6a2f247bc55bceba4259e983673f58b7"

// Rei Manager
#define EXPECTED_SIZE_SECOND 0x29c
#define EXPECTED_HASH_SECOND                                                   \
	"6eafa78ef61acedcb19facd0387e42046a6614126782620244def709f9a84c7e"

typedef struct {
	unsigned size;
	const char *sha256;
} apk_sign_key_t;

#endif /* MANAGER_SIGN_H */
