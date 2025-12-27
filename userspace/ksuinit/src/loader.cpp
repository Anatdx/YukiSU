/**
 * ksuinit - Module Loader
 * 
 * Handles loading the KernelSU LKM module with symbol resolution.
 */

#include "loader.hpp"
#include "log.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <elf.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ksuinit {

namespace {

/**
 * RAII class to manage kptr_restrict setting
 */
class KptrGuard {
public:
    KptrGuard() {
        // Save original value
        std::ifstream ifs("/proc/sys/kernel/kptr_restrict");
        if (ifs.is_open()) {
            std::getline(ifs, original_value_);
        }
        
        // Set to 1 to allow reading kallsyms
        std::ofstream ofs("/proc/sys/kernel/kptr_restrict");
        if (ofs.is_open()) {
            ofs << "1";
        }
    }
    
    ~KptrGuard() {
        // Restore original value
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

/**
 * Parse /proc/kallsyms to get kernel symbol addresses
 */
std::unordered_map<std::string, uint64_t> parse_kallsyms() {
    KptrGuard guard;
    
    std::unordered_map<std::string, uint64_t> symbols;
    
    std::ifstream ifs("/proc/kallsyms");
    if (!ifs.is_open()) {
        KLOGE("Cannot open /proc/kallsyms");
        return symbols;
    }
    
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string addr_str, type, name;
        
        if (!(iss >> addr_str >> type >> name)) {
            continue;
        }
        
        uint64_t addr = 0;
        try {
            addr = std::stoull(addr_str, nullptr, 16);
        } catch (...) {
            continue;
        }
        
        // Strip version suffixes like "$..." or ".llvm...."
        auto pos = name.find('$');
        if (pos == std::string::npos) {
            pos = name.find(".llvm.");
        }
        if (pos != std::string::npos) {
            name = name.substr(0, pos);
        }
        
        symbols[name] = addr;
    }
    
    return symbols;
}

/**
 * Read entire file into a vector
 */
bool read_file(const char* path, std::vector<uint8_t>& buffer) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        KLOGE("Cannot open file: %s", path);
        return false;
    }
    
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    
    buffer.resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size)) {
        KLOGE("Cannot read file: %s", path);
        return false;
    }
    
    return true;
}

/**
 * Call init_module syscall
 */
int init_module_syscall(void* module_image, unsigned long len, const char* param_values) {
    return syscall(__NR_init_module, module_image, len, param_values);
}

} // anonymous namespace

bool load_module(const char* path) {
    // Check if we're PID 1 (init process)
    if (getpid() != 1) {
        KLOGE("Invalid process (not init)");
        return false;
    }
    
    // Read the module file
    std::vector<uint8_t> buffer;
    if (!read_file(path, buffer)) {
        return false;
    }
    
    // Parse ELF header
    if (buffer.size() < sizeof(Elf64_Ehdr)) {
        KLOGE("File too small to be an ELF");
        return false;
    }
    
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buffer.data());
    
    // Verify ELF magic
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        KLOGE("Invalid ELF magic");
        return false;
    }
    
    // We only support 64-bit ELF
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        KLOGE("Only 64-bit ELF supported");
        return false;
    }
    
    // Parse kallsyms
    auto kernel_symbols = parse_kallsyms();
    if (kernel_symbols.empty()) {
        KLOGE("Cannot parse kallsyms");
        return false;
    }
    
    // Find symbol table section
    auto* shdr_base = reinterpret_cast<Elf64_Shdr*>(buffer.data() + ehdr->e_shoff);
    
    Elf64_Shdr* symtab = nullptr;
    Elf64_Shdr* strtab = nullptr;
    
    for (int i = 0; i < ehdr->e_shnum; i++) {
        auto* shdr = &shdr_base[i];
        if (shdr->sh_type == SHT_SYMTAB) {
            symtab = shdr;
            // String table is linked in sh_link
            strtab = &shdr_base[shdr->sh_link];
            break;
        }
    }
    
    if (!symtab || !strtab) {
        KLOGE("Cannot find symbol table");
        return false;
    }
    
    // Get pointers to symbol and string tables
    auto* sym_base = reinterpret_cast<Elf64_Sym*>(buffer.data() + symtab->sh_offset);
    auto* str_base = reinterpret_cast<char*>(buffer.data() + strtab->sh_offset);
    
    size_t sym_count = symtab->sh_size / sizeof(Elf64_Sym);
    
    // Resolve undefined symbols
    for (size_t i = 1; i < sym_count; i++) {
        auto* sym = &sym_base[i];
        
        // Only process undefined symbols
        if (sym->st_shndx != SHN_UNDEF) {
            continue;
        }
        
        // Get symbol name
        const char* name = &str_base[sym->st_name];
        if (!name || !*name) {
            continue;
        }
        
        // Look up in kernel symbols
        auto it = kernel_symbols.find(name);
        if (it == kernel_symbols.end()) {
            KLOGW("Cannot find symbol: %s", name);
            continue;
        }
        
        // Patch the symbol
        sym->st_shndx = SHN_ABS;
        sym->st_value = it->second;
    }
    
    // Load the module
    if (init_module_syscall(buffer.data(), buffer.size(), "") != 0) {
        KLOGE("init_module failed: %s", strerror(errno));
        return false;
    }
    
    KLOGI("Module loaded successfully");
    return true;
}

} // namespace ksuinit
