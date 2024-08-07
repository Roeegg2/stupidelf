#include "../include/parser.hpp"
#include "../include/runner.hpp"
#include "../include/syscalls.hpp"

#include <sys/mman.h>

using namespace Roee_ELF;

int main() {
    Parser_64b* parsed_info = reinterpret_cast<Parser_64b*>(syscall_mmap(0x0, sizeof(Parser_64b), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Runner* runner = reinterpret_cast<Runner*>(syscall_mmap(0x0, sizeof(Runner), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    parsed_info->init("tests/pic_hello");
    runner->init(parsed_info);

    parsed_info->parse_elf_header();
    parsed_info->parse_prog_headers();
    parsed_info->parse_sect_headers();
#ifdef DEBUG
    parsed_info->print_file_info();
    parsed_info->print_prog_headers();
    parsed_info->print_sect_headers();
#endif
    runner->run();

    syscall_exit(0);
}
