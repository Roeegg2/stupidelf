#include "../include/utils.hpp"

#include <sys/mman.h>

namespace Goblin {

inline void IDs::free_id(const id_t id) { m_free_ids.push(id); }

id_t IDs::allocate_id() {
    if (m_free_ids.empty()) {
        m_biggest_allocated++;
        return m_biggest_allocated;
    } else { // just repurpose a free used one
        id_t foo = m_free_ids.front();
        m_free_ids.pop();
        return foo;
    }
}

bool find_file(const std::filesystem::path &directory, const std::string &filename, std::string &found_path) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().filename() == filename) {
            found_path = entry.path().string();
            return true;
        }
    }
    return false;
}

// uint32_t get_page_count(const Elf64_Xword memsz, const Elf64_Addr addr) { return (memsz + (addr % PAGE_SIZE) + PAGE_SIZE - 1) /
// PAGE_SIZE; }

uint32_t get_page_count(const Elf64_Xword memsz, const Elf64_Addr addr) {
    auto actual_size = memsz + (addr & (PAGE_SIZE - 1));
    if (actual_size & (PAGE_SIZE - 1)) {
        return (actual_size / PAGE_SIZE) + 1;
    }
    return actual_size / PAGE_SIZE;
}

Elf64_Addr page_align_down(const Elf64_Addr addr) { return addr & (~(PAGE_SIZE - 1)); }

unsigned long elf_hash(const unsigned char *name) {
    unsigned long hash = 0;
    unsigned long g;

    while (*name) {
        hash = (hash << 4) + *name++;
        g = hash & 0xF0000000;
        if (g != 0)
            hash ^= g >> 24;
        hash &= ~g;
    }

    return hash;
}

uint32_t gnu_hash(const uint8_t *name) {
    uint32_t h = 5381;

    for (; *name; name++) {
        h = (h << 5) + h + *name;
    }

    return h;
}

int elf_perm_to_mmap_perms(const uint32_t elf_flags) {
    int mmap_flags = 0;

    if (elf_flags & 0x1)
        mmap_flags |= PROT_EXEC;
    if (elf_flags & 0x2)
        mmap_flags |= PROT_WRITE;
    if (elf_flags & 0x4)
        mmap_flags |= PROT_READ;

    return mmap_flags;
}

} // namespace Goblin
