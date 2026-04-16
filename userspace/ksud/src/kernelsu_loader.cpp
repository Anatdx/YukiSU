#include "kernelsu_loader.hpp"

#include "log.hpp"

#include <elf.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ksud::kernelsu_loader {

namespace {

class KptrGuard {
public:
    KptrGuard() {
        std::ifstream ifs("/proc/sys/kernel/kptr_restrict");
        if (ifs.is_open()) {
            std::getline(ifs, original_value_);
        }

        std::ofstream ofs("/proc/sys/kernel/kptr_restrict");
        if (ofs.is_open()) {
            ofs << "1";
        }
    }

    ~KptrGuard() {
        if (!original_value_.empty()) {
            std::ofstream ofs("/proc/sys/kernel/kptr_restrict");
            if (ofs.is_open()) {
                ofs << original_value_;
            }
        }
    }

private:
    std::string original_value_;
};

void normalize_symbol_name(std::string* name) {
    size_t pos = name->find('$');
    if (pos == std::string::npos) {
        pos = name->find(".llvm.");
    }
    if (pos != std::string::npos) {
        name->resize(pos);
    }
}

bool read_file(const char* path, std::vector<uint8_t>* buffer) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOGE("loader: cannot open %s", path);
        return false;
    }

    const auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    buffer->resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(buffer->data()), size)) {
        LOGE("loader: cannot read %s", path);
        return false;
    }

    return true;
}

int init_module_syscall(void* module_image, unsigned long len, const char* param_values) {
    return syscall(__NR_init_module, module_image, len, param_values);
}

template <typename Ehdr, typename Shdr, typename Sym>
bool patch_undefined_symbols(std::vector<uint8_t>* buffer) {
    if (buffer->size() < sizeof(Ehdr)) {
        LOGE("loader: file too small to be an ELF");
        return false;
    }

    auto* ehdr = reinterpret_cast<Ehdr*>(buffer->data());
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("loader: invalid ELF magic");
        return false;
    }

    auto* shdr_base = reinterpret_cast<Shdr*>(buffer->data() + ehdr->e_shoff);
    Shdr* symtab = nullptr;
    Shdr* strtab = nullptr;

    for (int i = 0; i < ehdr->e_shnum; ++i) {
        auto* shdr = &shdr_base[i];
        if (shdr->sh_type == SHT_SYMTAB) {
            symtab = shdr;
            strtab = &shdr_base[shdr->sh_link];
            break;
        }
    }

    if (symtab == nullptr || strtab == nullptr) {
        LOGE("loader: cannot find symbol table");
        return false;
    }

    auto* sym_base = reinterpret_cast<Sym*>(buffer->data() + symtab->sh_offset);
    auto* str_base = reinterpret_cast<char*>(buffer->data() + strtab->sh_offset);
    const size_t sym_count = symtab->sh_size / sizeof(Sym);
    std::unordered_map<std::string, std::vector<Sym*>> unresolved_symbols;

    for (size_t i = 1; i < sym_count; ++i) {
        auto* sym = &sym_base[i];
        if (sym->st_shndx != SHN_UNDEF) {
            continue;
        }

        const char* name = &str_base[sym->st_name];
        if (name == nullptr || *name == '\0') {
            continue;
        }
        unresolved_symbols[name].push_back(sym);
    }

    if (unresolved_symbols.empty()) {
        return true;
    }

    KptrGuard guard;
    std::ifstream ifs("/proc/kallsyms");
    if (!ifs.is_open()) {
        LOGE("loader: cannot open /proc/kallsyms");
        return false;
    }

    std::string line;
    while (!unresolved_symbols.empty() && std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string addr_str;
        std::string type;
        std::string name;
        std::string module_name;
        if (!(iss >> addr_str >> type >> name)) {
            continue;
        }

        // Kernel symbols come before module symbols in /proc/kallsyms. Stop scanning once we
        // reach module-owned entries so we don't accidentally relocate against them.
        if (iss >> module_name) {
            break;
        }

        uint64_t addr = 0;
        try {
            addr = std::stoull(addr_str, nullptr, 16);
        } catch (...) {
            continue;
        }

        normalize_symbol_name(&name);

        const auto it = unresolved_symbols.find(name);
        if (it == unresolved_symbols.end()) {
            continue;
        }

        for (auto* sym : it->second) {
            sym->st_shndx = SHN_ABS;
            sym->st_value = static_cast<decltype(sym->st_value)>(addr);
        }
        unresolved_symbols.erase(it);
    }

    if (!ifs.eof() && ifs.fail()) {
        LOGE("loader: failed while reading /proc/kallsyms");
        return false;
    }

    for (const auto& [name, _] : unresolved_symbols) {
        LOGW("loader: cannot find symbol: %s", name.c_str());
    }

    return true;
}

}  // namespace

bool load_module(const char* path, const std::string& param_values) {
    std::vector<uint8_t> buffer;
    if (!read_file(path, &buffer)) {
        return false;
    }

    if (buffer.size() < EI_NIDENT || memcmp(buffer.data(), ELFMAG, SELFMAG) != 0) {
        LOGE("loader: invalid ELF image");
        return false;
    }

    const unsigned char elf_class = buffer[EI_CLASS];
    bool patched = false;
    if (elf_class == ELFCLASS64) {
        patched = patch_undefined_symbols<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(&buffer);
    } else if (elf_class == ELFCLASS32) {
        patched = patch_undefined_symbols<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(&buffer);
    } else {
        LOGE("loader: unsupported ELF class %u", elf_class);
        return false;
    }

    if (!patched) {
        return false;
    }

    if (init_module_syscall(buffer.data(), buffer.size(), param_values.c_str()) != 0) {
        if (errno == EEXIST) {
            return true;
        }
        LOGE("loader: init_module failed: %s", strerror(errno));
        return false;
    }

    LOGI("loader: module loaded successfully");
    return true;
}

}  // namespace ksud::kernelsu_loader
