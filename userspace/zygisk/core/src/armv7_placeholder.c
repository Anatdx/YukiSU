/*
 * Reserved armv7 YukiZygisk payload.
 *
 * zygote32 support is intentionally not implemented yet.  Keep this inert
 * shared object so the armv7 artifact and packaging contracts exist without
 * exposing a partial implementation to a 32-bit zygote.
 */
__attribute__((used, visibility("hidden"))) void
yuki_zygisk_armv7_placeholder(void) {}
