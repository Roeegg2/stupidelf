#include "../include/loadable.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
char **environ;
};

namespace Goblin {
const char *Loadable::s_DEFAULT_SHARED_OBJ_PATHS[]{
    "/lib/",
    "/usr/lib/",
    "/lib64/",
    "/usr/lib64/",
};
std::vector<std::shared_ptr<Loadable>> Loadable::s_loaded_dependencies;

Loadable::Loadable(const std::string file_path)
    : ELF_File(file_path), m_dyn_seg_index(-1), m_tls_seg_index(-1), m_rpath(nullptr), m_runpath(nullptr), m_dyn({{0, 0}, {0, 0}, 0, 0}),
      m_plt({{0, 0}, 0}),
      m_sht_indices({get_section_index_by_type(SHT_DYNSYM), get_section_index_by_type(SHT_DYNSYM),
                     get_section_index_by_type(SHT_DYNSYM)}) // ELF_File calls full_parse on contructor so there is not issue here
{
    m_sht_indices.dynsym = get_section_index_by_type(SHT_DYNSYM);
    m_sht_indices.elf_hash = get_section_index_by_type(SHT_HASH);
    m_sht_indices.gnu_hash = get_section_index_by_type(SHT_GNU_HASH);
    m_segment_data.reserve(m_elf_header.e_phnum);
}

Loadable::~Loadable(void) {}

Elf64_Sym *Loadable::lookup_regular_dynsym(const char *sym_name) const {
    static const auto ent_num = m_sect_headers[m_sht_indices.dynsym].sh_size / m_sect_headers[m_sht_indices.dynsym].sh_entsize;

    return get_sym_by_name(m_dyn.sym_table, m_dyn.str_table, sym_name, ent_num);
}

Elf64_Sym *Loadable::lookup_gnu_hash_dynsym(const char *sym_name) const {
    const uint32_t namehash = gnu_hash(reinterpret_cast<const unsigned char *>(sym_name));

    uint64_t word = m_hash_data.bloom[(namehash / 64) % m_hash_data.bloom_size];
    uint64_t mask = 0 | (uint64_t)1 << (namehash % 64) | (uint64_t)1 << ((namehash >> m_hash_data.bloom_shift) % 64);

    if ((word & mask) != mask) {
        return nullptr;
    }

    uint32_t symix = m_hash_data.buckets[namehash % m_hash_data.nbuckets];
    if (symix < m_hash_data.symoffset) {
        return nullptr;
    }

    while (true) {
        const char *symname = m_dyn.str_table + m_dyn.sym_table[symix].st_name;
        const uint32_t hash = m_hash_data.chains[symix - m_hash_data.symoffset];

        if ((namehash | 1) == (hash | 1) && strcmp(sym_name, symname) == 0) {
            return &m_dyn.sym_table[symix];
        }

        /* Chain ends with an element with the lowest bit set to 1. */
        if (hash & 1) {
            break;
        }

        symix++;
    }

    return nullptr;
}

Elf64_Sym *Loadable::lookup_elf_hash_dynsym(const char *sym_name) const {
    const uint32_t namehash = elf_hash(reinterpret_cast<const unsigned char *>(sym_name));
    // FIXME: might be more efficient to hash and then compare hashes?
    for (uint32_t i = m_hash_data.buckets[namehash % m_hash_data.nbuckets]; i != STN_UNDEF; i = m_hash_data.chains[i]) {
        if (std::strcmp(m_dyn.str_table + m_dyn.sym_table[i].st_name, sym_name) == 0) {
            return m_dyn.sym_table + i;
        }
    }

    return nullptr;
}

void Loadable::parse_dyn_segment(std::set<Elf64_Xword> &dt_needed_syms) {
    Elf64_Dyn *dyn_table = reinterpret_cast<Elf64_Dyn *>(m_segment_data[m_dyn_seg_index]);
    while (dyn_table->d_tag != DT_NULL) {
        switch (dyn_table->d_tag) {
        case DT_RELA: // m_dyn rela table
            m_dyn.rela.m_addr = reinterpret_cast<Elf64_Rela *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_JMPREL: // PLT relocation table
            m_plt.rela.m_addr = reinterpret_cast<Elf64_Rela *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_SYMTAB: // m_dyn symbol table
            m_dyn.sym_table = reinterpret_cast<Elf64_Sym *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_STRTAB: // m_dyn string table
            m_dyn.str_table = reinterpret_cast<char *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_RELASZ: // m_dyn rela table total size
            m_dyn.rela.m_total_size = dyn_table->d_un.d_val;
            break;
        case DT_PLTRELSZ: // PLT relocation table total size
            m_plt.rela.m_total_size = dyn_table->d_un.d_val;
            break;
        case DT_PLTGOT: // m_plt.got address
            m_plt.got = reinterpret_cast<Elf64_Addr *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_NEEDED: // name of a shared object we need to load
            dt_needed_syms.insert(dyn_table->d_un.d_val);
            break;
        case DT_RPATH:
            m_rpath = reinterpret_cast<char *>(dyn_table->d_un.d_val);
            break;
        case DT_RUNPATH:
            m_runpath = reinterpret_cast<char *>(dyn_table->d_un.d_val);
            break;
        case DT_RELR:
            m_dyn.relr.m_addr = reinterpret_cast<Elf64_Relr *>(dyn_table->d_un.d_ptr + m_load_base_addr);
            break;
        case DT_RELRSZ:
            m_dyn.relr.m_total_size = dyn_table->d_un.d_val;
            break;
        }
        dyn_table++;
    }

    // doing this later because we need to get m_dyn.str_table first
    if (m_rpath != nullptr) {
        m_rpath =
            reinterpret_cast<Elf64_Addr>(m_dyn.str_table) + m_rpath; // adding the offset (current value of m_rpath) with the dynstr table
    }
    if (m_runpath != nullptr) {
        m_runpath = reinterpret_cast<Elf64_Addr>(m_dyn.str_table) +
                    m_runpath; // adding the offset (current value of  m_runpath) with the dynstr table
    }
}

bool Loadable::check_n_handle_loaded_dep(const Elf64_Xword dt_needed) {
    const uint16_t foo_size = s_loaded_dependencies.size();
    for (uint16_t i = 0; i < foo_size; i++) {
        if (s_loaded_dependencies[i]->m_elf_file_path.filename() == m_dyn.str_table + dt_needed) { // if shared lib is already been loaded
            m_dependencies.insert(s_loaded_dependencies[i]);
            return true;
        }
    }

    return false;
}

bool Loadable::check_n_handle_new_dep(const Elf64_Xword dt_needed) {
    std::string path;
    if (resolve_path_rpath_runpath(m_rpath, path, m_dyn.str_table + dt_needed) ||
        resolve_path_rpath_runpath(m_runpath, path, m_dyn.str_table + dt_needed) ||
        resolve_path_ld_library_path(path, m_dyn.str_table + dt_needed) || resolve_path_default(path, m_dyn.str_table + dt_needed)) {

        s_loaded_dependencies.emplace_back(new Loadable(path));
        m_dependencies.insert(s_loaded_dependencies.back());

        return true;
    }

    return false;
}

inline void Loadable::construct_loadables_for_shared_objects(const std::set<Elf64_Xword> &dt_needed_syms) {
    for (Elf64_Xword dt_needed : dt_needed_syms) { // for each SO
        if (check_n_handle_loaded_dep(dt_needed) || check_n_handle_new_dep(dt_needed)) {
            continue;
        }

        _GOBLIN_PRINT_ERR("Failed to resolve path for shared object" << m_dyn.str_table + dt_needed);
    }
}

bool Loadable::resolve_path_rpath_runpath(const char *r_run_path, std::string &path, const char *shared_obj_name) const {
    if ((r_run_path != nullptr) && (std::strcmp(r_run_path, "$ORIGIN") == 0)) {
        if (find_file(m_elf_file_path.parent_path(), shared_obj_name, path)) {
            return true;
        }
    }

    return false;
}

bool Loadable::resolve_path_ld_library_path(std::string &path, const char *shared_obj_name) {
    char *ld_library_path;
    if (((ld_library_path = getenv("LD_LIBRARY_PATH")) == NULL) || (ld_library_path[0] == '\0')) {
        return false;
    }

    return find_file(ld_library_path, shared_obj_name, path);
}

bool Loadable::resolve_path_default(std::string &path, const char *shared_obj_name) {
    for (const char *default_dir : s_DEFAULT_SHARED_OBJ_PATHS) {
        if (find_file(default_dir, shared_obj_name, path)) {
            return true;
        }
    }

    return false;
}

void Loadable::alloc_mem_for_segments(void) {
    {
        int8_t i = 0;
        while (((m_elf_header.e_phnum > i) && (m_prog_headers[i].p_type != PT_LOAD))) {
            i++;
        }
        m_load_base_addr = m_prog_headers[i].p_vaddr; // 0 for PIE, actual base address for non-PIE. that way
                                                      // mmap will allocate memory at the correct address (0
                                                      // means kernel will choose the address for us)
        while (((m_elf_header.e_phnum > i) && (m_prog_headers[i].p_type == PT_LOAD))) {
            i++;
        }
        uint32_t total_page_count =
            get_page_count(m_prog_headers[i - 1].p_vaddr + m_prog_headers[i - 1].p_memsz - m_load_base_addr, m_load_base_addr) + 1;

        m_load_base_addr = reinterpret_cast<Elf64_Addr>(mmap(reinterpret_cast<void *>(m_load_base_addr), total_page_count * PAGE_SIZE,
                                                             PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    }

    if (reinterpret_cast<void *>(m_load_base_addr) == MAP_FAILED) {
        _GOBLIN_PRINT_ERR_INTERNAL("mmap failed")
    }

    if (m_elf_header.e_type == ET_EXEC)
        m_load_base_addr = 0x0;
}

void Loadable::setup_segment(const Elf64_Word i) {
    m_segment_data[i] = reinterpret_cast<void *>(m_prog_headers[i].p_vaddr + m_load_base_addr); // mmap assigns only page aligned addresses,
                                                                                                // and we need to know the exact address
    m_elf_file.seekg(m_prog_headers[i].p_offset);
    m_elf_file.read(reinterpret_cast<char *>(m_segment_data[i]),
                    m_prog_headers[i].p_filesz); // read data from file to memory

    // As specified in the System V ABI, all data not mapped from file should be zeroed out
    if (m_prog_headers[i].p_filesz < m_prog_headers[i].p_memsz) {
        std::memset(reinterpret_cast<void *>(m_prog_headers[i].p_vaddr + m_load_base_addr + m_prog_headers[i].p_filesz), 0,
                    m_prog_headers[i].p_memsz - m_prog_headers[i].p_filesz);
    }
}

void Loadable::map_segments(struct tls *tls, const id_t mod_id) {
    alloc_mem_for_segments();

    for (int8_t i = 0; i < m_elf_header.e_phnum; i++) {
        if (m_prog_headers[i].p_memsz <= 0) {
            continue;
        }

        switch (m_prog_headers[i].p_type) {
        case PT_TLS:
            m_tls_seg_index = i;
            tls->m_init_imgs.emplace_back(mod_id, m_prog_headers[i].p_memsz, &m_segment_data[i], tls->m_total_imgs_size, true);
            tls->m_total_imgs_size += m_prog_headers[i].p_memsz;
            break;
        case PT_DYNAMIC:
            m_dyn_seg_index = i;
            break;
        case PT_GNU_RELRO:
            break;
        case PT_LOAD:
            break;
        default: // nothing to do, so go next
            continue;
        }

        setup_segment(i);
    }
}

void Loadable::set_correct_permissions(void) {
    for (int8_t i = 0; i < m_elf_header.e_phnum; i++) {
        if (((m_prog_headers[i].p_type == PT_LOAD) && (m_prog_headers[i].p_memsz > 0))) {
            if (m_prog_headers[i].p_memsz > 0) {
                const uint16_t page_count = get_page_count(m_prog_headers[i].p_memsz, m_prog_headers[i].p_vaddr);

                if (mprotect(reinterpret_cast<void *>(page_align_down(reinterpret_cast<Elf64_Addr>(m_segment_data[i]))),
                             page_count * PAGE_SIZE, elf_perm_to_mmap_perms(m_prog_headers[i].p_flags)) == -1) {
                    _GOBLIN_PRINT_ERR_INTERNAL("mprotect failed")
                }
            }
        }
    }
}

void Loadable::handle_if_module_is_glibc(struct executable_shared &exec_shared, const id_t mod_id) const {
    // NOTE: not sure if I should use the filename or the full path
    auto res = m_elf_file_path.generic_string().find("libc.so");
    // auto res = m_elf_file_path.filename().generic_string().find("libc.so");

    if (res != std::string::npos) { // if this is glibc
        if (exec_shared.m_glibc_modid != 0) {
            _GOBLIN_PRINT_WARN("Loaded glibc multiple times, something probably went wrong");
        }
        exec_shared.m_glibc_modid = mod_id;
    }
}

void Loadable::build_shared_objs_tree(struct executable_shared &exec_shared) {
    {
        const id_t mod_id = exec_shared.m_mod_ids.allocate_id();
        handle_if_module_is_glibc(exec_shared, mod_id);

        _GOBLIN_PRINT_INFO("Loading shared object " << m_elf_file_path << " and assigning module ID: " << mod_id);

        map_segments(&exec_shared.m_tls, mod_id);
    }

    if (m_dyn_seg_index >= 0) {
        std::set<Elf64_Xword> dt_needed_syms;
        parse_dyn_segment(dt_needed_syms);
        construct_loadables_for_shared_objects(dt_needed_syms); // for each shared object dependency, create a Loadable object
    }
    {

        std::set<Elf64_Word> relas_copy, relas_jumps_globd, relas_tls_dtpmod64, relas_tls_tpoff64, relas_tls_dtpoff64;
        apply_dyn_relr_relocations();
        apply_dyn_rela_relocations(relas_copy, relas_jumps_globd, relas_tls_dtpmod64, relas_tls_tpoff64, relas_tls_dtpoff64);
        apply_plt_rela_relocations(relas_jumps_globd, exec_shared.m_options.binding);

        for (auto &dep : m_dependencies) { // for every shared object dependency recursively build the shared objects tree
            dep->build_shared_objs_tree(exec_shared);
            apply_external_dyn_relocations(dep.get(), relas_copy, relas_jumps_globd, relas_tls_dtpmod64,
                                           exec_shared.m_options.symbol_resolution);
        }
    }

    // set_correct_permissions(); // set the correct permissions for the segments
}

/* for anyone reading this in the future, I'm very sorry for this
 * mess...Hopefully you pull through :) */
uint8_t Loadable::set_sym_lookup_method(const uint8_t symbol_resolution_option) {
    if (__builtin_expect(symbol_resolution_option == SYMBOL_RESOLUTION_GNU_HASH, 1)) { // if user specified GNU hash
        if (m_sht_indices.gnu_hash == (uint16_t)(-1)) {                                // if no .gnu.hash section found
            _GOBLIN_PRINT_WARN("No .gnu.hash section found, trying ELF hash");
            goto try_elf_hash;
        }
        f_lookup_dynsym =
            std::bind(&Loadable::lookup_gnu_hash_dynsym, this, std::placeholders::_1); // otherwise, set GNU hash lookup method
        return SYMBOL_RESOLUTION_GNU_HASH;
    }
    if (__builtin_expect(symbol_resolution_option == SYMBOL_RESOLUTION_ELF_HASH, 1)) { // if user specified ELF hash
    try_elf_hash:
        if (m_sht_indices.elf_hash == (uint16_t)(-1)) { // if no .hash section found
            _GOBLIN_PRINT_WARN("No .hash section found, trying symtab");
            goto try_symtab;
        }
        f_lookup_dynsym = std::bind(&Loadable::lookup_elf_hash_dynsym, this, std::placeholders::_1); // set ELF hash lookup method
        return SYMBOL_RESOLUTION_ELF_HASH;
    }

try_symtab:
    f_lookup_dynsym = std::bind(&Loadable::lookup_regular_dynsym, this, std::placeholders::_1); // set symtab lookup method
    return SYMBOL_RESOLUTION_SYMTAB;
}

void Loadable::init_hash_tab_data(const uint8_t lookup_method) {
    uint8_t index;
    if (lookup_method == SYMBOL_RESOLUTION_SYMTAB) { // no data to read!
        return;
    }

    // individual stuff
    if (lookup_method == SYMBOL_RESOLUTION_GNU_HASH) {
        index = m_sht_indices.gnu_hash;
        m_hash_data.nbuckets = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[0];
        m_hash_data.symoffset = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[1];
        m_hash_data.bloom_size = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[2];
        m_hash_data.bloom_shift = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[3];
        m_hash_data.bloom = &reinterpret_cast<uint64_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[4];
        m_hash_data.buckets = &reinterpret_cast<uint32_t *>(m_hash_data.bloom)[m_hash_data.bloom_size];
        m_hash_data.chains = &m_hash_data.buckets[m_hash_data.nbuckets];
    } else { // its ELF hash
        index = m_sht_indices.elf_hash;
        m_hash_data.nbuckets = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[0];
        m_hash_data.nchains = reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[1];
        m_hash_data.buckets = &reinterpret_cast<uint32_t *>(m_sect_headers[index].sh_addr + m_load_base_addr)[2];
        m_hash_data.chains = &m_hash_data.buckets[m_hash_data.nbuckets];
    }
}

void Loadable::apply_external_dyn_relocations(Loadable *dep, const std::set<Elf64_Word> &relas_copy,
                                              const std::set<Elf64_Word> &relas_jumps_globd,
                                              __attribute__((unused)) const std::set<Elf64_Word> &relas_tls_dtpmod64,
                                              const uint8_t symbol_resolution_option) {
    const uint8_t lookup_method = dep->set_sym_lookup_method(symbol_resolution_option);
    dep->init_hash_tab_data(lookup_method);

    for (auto sym_index : relas_copy) {                                                  // for every needed symbol
        std::string org_sym_name = m_dyn.str_table + m_dyn.sym_table[sym_index].st_name; // get name of original symbol

        Elf64_Sym *sym = dep->f_lookup_dynsym(org_sym_name.c_str()); // loop up the symbol

        if (sym != nullptr) {
            char *src = reinterpret_cast<char *>(m_dyn.sym_table[sym_index].st_value + m_load_base_addr);
            const char *dst = reinterpret_cast<const char *>(sym->st_value + dep->m_load_base_addr);
            std::strcpy(src, dst);
        } else {
            _GOBLIN_PRINT_WARN("Couldn't find definition for external symbol: " << org_sym_name);
        }
    }
    for (auto sym_index : relas_jumps_globd) {
        std::string org_sym_name = m_dyn.str_table + m_dyn.sym_table[sym_index].st_name; // get name of original symbol

        {
            const size_t pos = org_sym_name.find('@');
            if (pos != std::string::npos) {
                org_sym_name.insert(pos, 1, '@');
            }
        }

        Elf64_Sym *sym = dep->f_lookup_dynsym(org_sym_name.c_str()); // loop up the symbol

        Elf64_Addr *addr = reinterpret_cast<Elf64_Addr *>(m_dyn.rela.m_addr[sym_index].r_offset +
                                                          m_load_base_addr); // get address where we need to apply relocation
        *addr = sym->st_value + dep->m_load_base_addr;
    }
}

void Loadable::apply_tls_relocations(void) { _GOBLIN_PRINT_INFO("No TLS relocations found"); }

void Loadable::apply_plt_rela_relocations(std::set<Elf64_Word> relas_jumps_globd, const uint8_t binding_option) {
    if (m_plt.rela.m_addr == nullptr) {
        _GOBLIN_PRINT_INFO("No PLT relocations found");
        return;
    }

    _GOBLIN_PRINT_INFO("Applying PLT relocations");

    if (__builtin_expect(binding_option == BINDING_LAZY, 1)) {
        return;
    } else { // eager binding
        for (Elf64_Word i = 0; i < (m_plt.rela.m_total_size / m_plt.rela.s_ENTRY_SIZE); i++) {
            Elf64_Addr *addr = reinterpret_cast<Elf64_Addr *>(m_plt.rela.m_addr[i].r_offset + m_load_base_addr);
            /*Elf64_Sym *sym = reinterpret_cast<Elf64_Sym *>(m_plt.sym_table + ELF64_R_SYM(m_dyn.rela.m_addr[i].r_info));*/

            switch (ELF64_R_TYPE(m_plt.rela.m_addr[i].r_info)) {
            case R_X86_64_JUMP_SLOT:
            case R_X86_64_GLOB_DAT: // simply copy the value of the symbol to the address
                relas_jumps_globd.insert(ELF64_R_SYM(m_dyn.rela.m_addr[i].r_info));
                break;
            case R_X86_64_IRELATIVE: // interpret rela.addr[i].r_addend +
                *addr =
                    reinterpret_cast<Elf64_Addr>(reinterpret_cast<Elf64_Addr *(*)()>(m_plt.rela.m_addr[i].r_addend + m_load_base_addr)());
                break;
            default:
                _GOBLIN_PRINT_WARN("PLT Unknown relocation type: " << std::dec << ELF64_R_TYPE(m_plt.rela.m_addr[i].r_info));
            }
        }
    }
}

/*relatively new feature added to ELF*/
void Loadable::apply_dyn_relr_relocations(void) {
    if (m_dyn.relr.m_addr == nullptr) {
        _GOBLIN_PRINT_INFO("No RELR relocations found");
        return;
    }

    _GOBLIN_PRINT_INFO("Applying RELR relocations");

    for (Elf64_Word i = 0; i < (m_dyn.relr.m_total_size / m_dyn.relr.s_ENTRY_SIZE); i++) {
        m_dyn.relr.m_addr[i] += m_load_base_addr;
    }
}

void Loadable::apply_dyn_rela_relocations(std::set<Elf64_Word> &relas_copy, std::set<Elf64_Word> &relas_jumps_globd,
                                          std::set<Elf64_Word> &relas_tls_dtpmod64, std::set<Elf64_Word> &relas_tls_tpoff64,
                                          std::set<Elf64_Word> &relas_tls_dtpoff64) {
    if (m_dyn.rela.m_addr == nullptr) {
        _GOBLIN_PRINT_INFO("No RELA relocations found");
        return;
    }
    _GOBLIN_PRINT_INFO("Applying RELA relocations");

    for (Elf64_Word i = 0; i < (m_dyn.rela.m_total_size / m_dyn.rela.s_ENTRY_SIZE); i++) {
        Elf64_Addr *addr = reinterpret_cast<Elf64_Addr *>(m_dyn.rela.m_addr[i].r_offset + m_load_base_addr);
        Elf64_Sym *sym = reinterpret_cast<Elf64_Sym *>(m_dyn.sym_table + ELF64_R_SYM(m_dyn.rela.m_addr[i].r_info));

        switch (ELF64_R_TYPE(m_dyn.rela.m_addr[i].r_info)) {
        case R_X86_64_RELATIVE:
            *addr = m_dyn.rela.m_addr[i].r_addend + m_load_base_addr;
            break;
        case R_X86_64_64:
            *addr = m_dyn.rela.m_addr[i].r_addend + sym->st_value + m_load_base_addr;
            break;
        case R_X86_64_COPY: // advacned relocation type (data is needed from external object)
            relas_copy.insert(ELF64_R_SYM(m_dyn.rela.m_addr[i].r_info));
            break;
        case R_X86_64_IRELATIVE: // interpret rela.addr[i].r_addend +
            *addr = reinterpret_cast<Elf64_Addr>(reinterpret_cast<Elf64_Addr *(*)()>(m_dyn.rela.m_addr[i].r_addend + m_load_base_addr)());
            break;
        case R_X86_64_GLOB_DAT: // simply copy the value of the symbol to the address
            relas_jumps_globd.insert(ELF64_R_SYM(m_dyn.rela.m_addr[i].r_info));
            break;
        case R_X86_64_DTPMOD64:
            relas_tls_dtpmod64.insert(i);
            break;
        case R_X86_64_DTPOFF64:
            relas_tls_dtpoff64.insert(i);
            break;
        case R_X86_64_TPOFF64:
            relas_tls_tpoff64.insert(i);
            break;
        default:
            _GOBLIN_PRINT_WARN("Unknown relocation type number: " << std::dec << ELF64_R_TYPE(m_dyn.rela.m_addr[i].r_info));
        }
    }
}
}; // namespace Goblin
