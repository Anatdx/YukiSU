#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/system_properties.h>

struct prop_info {};

static struct prop_info g_adb_root_prop;

extern "C" [[gnu::visibility("default"), gnu::used]]
int __android_log_is_debuggable() {
  return 1;
}

[[gnu::visibility("default"), gnu::used]]
std::string
GetProperty(const std::string &key, const std::string &default_value) asm(
    "_ZN7android4base11GetPropertyERKNSt3__112basic_stringIcNS1_11char_"
    "traitsIcEENS1_9allocatorIcEEEES9_");

[[gnu::visibility("default"), gnu::used]]
std::string GetProperty(const std::string &key,
                        const std::string &default_value) {
  static constexpr const char *kSymbolName =
      "_ZN7android4base11GetPropertyERKNSt3__112basic_stringIcNS1_11char_"
      "traitsIcEENS1_9allocatorIcEEEES9_";
  static auto orig_fn =
      reinterpret_cast<decltype(GetProperty) *>(dlsym(RTLD_NEXT, kSymbolName));

  if (!orig_fn) {
    return default_value;
  }

  if (key == "service.adb.root") {
    return orig_fn(key, "1");
  }

  return orig_fn(key, default_value);
}

extern "C" [[gnu::visibility("default"), gnu::used]]
const prop_info *__system_property_find(const char *name) {
  using Fn = const prop_info *(*)(const char *);
  static auto orig_fn =
      reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "__system_property_find"));

  if (std::strcmp(name, "service.adb.root") == 0) {
    return &g_adb_root_prop;
  }

  return orig_fn ? orig_fn(name) : nullptr;
}

extern "C" [[gnu::visibility("default"), gnu::used]]
void __system_property_read_callback(const prop_info *pi,
                                     void (*callback)(void *cookie,
                                                      const char *name,
                                                      const char *value,
                                                      uint32_t serial),
                                     void *cookie) {
  using Fn =
      void (*)(const prop_info *,
               void (*)(void *, const char *, const char *, uint32_t), void *);
  static auto orig_fn =
      reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "__system_property_read_callback"));

  if (pi == &g_adb_root_prop) {
    if (callback) {
      callback(cookie, "service.adb.root", "1", 0);
    }
    return;
  }

  if (orig_fn) {
    orig_fn(pi, callback, cookie);
  }
}

extern "C" [[gnu::visibility("default"), gnu::used]]
int selinux_android_setcon(const char *con) {
  (void)con;
  return 0;
}

[[gnu::used, gnu::constructor(0)]]
void Init() {
  unsetenv("LD_PRELOAD");
  unsetenv("LD_LIBRARY_PATH");
  std::string path = getenv("PATH") ? getenv("PATH") : "";
  if (!path.empty()) {
    path += ":/data/adb/ksu/bin";
  } else {
    path = "/data/adb/ksu/bin";
  }
  setenv("PATH", path.c_str(), 1);
}
