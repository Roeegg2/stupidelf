#include "../include/runner.hpp"
#include "../include/utils.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <iostream>

namespace Roee_ELF {
    constexpr Elf64_Addr load_base_addr = 0x400000;

    Runner::Runner(const char* file_path) : Parser_64b(file_path) {
        elf_file_fd = open(file_path, O_RDONLY);
    }
    void Runner::map_pt_load_segment(const uint8_t i) {
        segment_data[i] = mmap(reinterpret_cast<void*>(prog_headers[i].p_vaddr), prog_headers[i].p_memsz,
            PROT_READ | PROT_WRITE, MAP_PRIVATE, elf_file_fd, PAGE_ALIGN_DOWN(prog_headers[i].p_offset));
        // if segments arent page aligned, we need to adjust the pointer after the mapping
        segment_data[i] = reinterpret_cast<void*>(prog_headers[i].p_vaddr);

        // as specified by the format, we need to zero out the memory that is not in the file
        memset(reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(segment_data[i]) + prog_headers[i].p_filesz),
            0x0, prog_headers[i].p_memsz - prog_headers[i].p_filesz);

        // change the permissions of the segment to what they should be
        if (mprotect(segment_data[i], prog_headers[i].p_memsz,
            elf_perm_to_mmap_perms(prog_headers[i].p_flags)) == -1) {
            std::cerr << "mprotect failed\n";
            exit(1);
        }
    }

    void Runner::map_segments(void) {
        dyn_seg_index = -1;
        segment_data = new void*[elf_header.e_phnum];
        for (int8_t i = 0; i < elf_header.e_phnum; i++) { // map the segments to memory
            switch (prog_headers[i].p_type) {
                case PT_LOAD:
                    map_pt_load_segment(i);
                    break;
                case PT_DYNAMIC:
                    dyn_seg_index = i;
                    // mmap_wrapper(&segment_data[i], 0x0, prog_headers[i].p_memsz, PROT_READ | PROT_WRITE,
                    //     MAP_PRIVATE, elf_file_fd, PAGE_ALIGN_DOWN(prog_headers[i].p_offset));
                    segment_data[i] = mmap(0x0, prog_headers[i].p_memsz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE, elf_file_fd, PAGE_ALIGN_DOWN(prog_headers[i].p_offset));
                    segment_data[i] = reinterpret_cast<void**>(reinterpret_cast<Elf64_Addr>(segment_data[i]) +
                        prog_headers[i].p_offset - PAGE_ALIGN_DOWN(prog_headers[i].p_offset));
                    break;
                default:
                    segment_data[i] = nullptr;
            }
        }
    }

    void Runner::run(void) { //elf_header.e_entry 0x401655
        map_segments();
#ifdef DEBUG
        print_dynamic_segment();
        std::cout << "Starting execution...\n";
#endif
        start_execution = reinterpret_cast<void(*)()>(elf_header.e_entry); // turn the code segment start into a function ptr
        start_execution(); // execute executable code
    }
}
