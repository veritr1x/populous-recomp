// loader.h - maps the original PE into the guest arena and starts it.
#pragma once
#include "guest.h"
#include <string>
#include <vector>

struct SectionInfo {
    std::string name;
    uint32_t va;       // guest virtual address
    uint32_t vsize;    // Misc_VirtualSize
    uint32_t raw_size; // SizeOfRawData
    uint32_t raw_ptr;  // PointerToRawData
    uint32_t characteristics;
};

// Default image and its expected content hash.
extern const char *const LOADER_DEFAULT_EXE; // "original/gog/D3DPopTB.exe"
extern const char *const LOADER_EXPECTED_SHA256;
// The SHA-256 of the image loader_load actually mapped, as lowercase hex, or
// "" before a load. Equal to LOADER_EXPECTED_SHA256 on a successful load; kept
// separate because it is a fact about this run rather than a build constant.
const char *loader_exe_sha256();
static const uint32_t LOADER_EXPECTED_ENTRY = 0x0055d6c0u;

// Maps the PE at `exe_path` (nullptr => LOADER_DEFAULT_EXE) into a freshly
// initialised guest arena: sections at their virtual addresses, tail of each
// section zero filled (.bss), PE headers at the image base, IAT patched with
// import trampolines, TEB/TLS/stack prepared. Refuses any image whose SHA-256
// does not match LOADER_EXPECTED_SHA256. There is no hash bypass.
// Returns false and leaves loader_error() set on failure.
bool loader_load(const char *exe_path = nullptr);

const char *loader_error();
uint32_t loader_image_base();
uint32_t loader_image_size();
// IMAGE_BASE + SizeOfImage, i.e. one past the last image byte. 0 before a load.
uint32_t loader_image_limit();
uint32_t loader_entry_point();
const std::vector<SectionInfo> &loader_sections();
const std::string &loader_exe_path();
// IAT slots patched: trampolines for code imports, guest storage for data ones.
uint32_t loader_iat_patched();
uint32_t loader_iat_data_imports();

// Resets `c` to the process-start state: zeroed registers, ESP just below
// STACK_TOP with a sentinel return address pushed, FS base at the TEB, x87
// control word 0x027f.
void loader_init_context(X86 *c);

// Process-wide context used by run_entry() and by hosts that do not keep their
// own X86 instance.
X86 *loader_context();

// Calls the PE entry point through recomp_call using loader_context().
void run_entry();
void run_entry(X86 *c);
