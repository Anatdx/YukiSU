#include <cstdlib>
#include <dlfcn.h>
#include <string>

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
