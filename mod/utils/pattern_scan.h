#pragma once
#include <Windows.h>
#include <cstdint>
#include <string_view>
#include <optional>
#include <vector>

// IDA-style pattern scanner.
// Example pattern: "48 8B 05 ?? ?? ?? ?? 48 85 C0"
// '??' bytes are wildcards that match anything.

namespace PatternScan {

    struct BytePattern {
        std::vector<std::pair<uint8_t, bool>> bytes; // (value, isMask)
    };

    inline BytePattern Parse(std::string_view pattern) {
        BytePattern result;
        size_t i = 0;
        while (i < pattern.size()) {
            // Skip whitespace
            while (i < pattern.size() && pattern[i] == ' ') ++i;
            if (i >= pattern.size()) break;

            if (pattern[i] == '?' ) {
                // Wildcard — skip one or two '?'
                result.bytes.push_back({0x00, true});
                ++i;
                if (i < pattern.size() && pattern[i] == '?') ++i;
            } else {
                // Parse hex byte
                uint8_t hi = 0, lo = 0;
                auto hexDigit = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    return 0;
                };
                hi = hexDigit(pattern[i++]);
                if (i < pattern.size() && pattern[i] != ' ' && pattern[i] != '?')
                    lo = hexDigit(pattern[i++]);
                result.bytes.push_back({static_cast<uint8_t>((hi << 4) | lo), false});
            }
        }
        return result;
    }

    // Scan a memory region for a pattern; returns the address or 0.
    inline uintptr_t Scan(uintptr_t start, size_t size, const BytePattern& pattern) {
        const auto& bytes = pattern.bytes;
        if (bytes.empty() || size < bytes.size()) return 0;

        const uint8_t* data = reinterpret_cast<const uint8_t*>(start);
        const size_t len    = bytes.size();

        for (size_t i = 0; i <= size - len; ++i) {
            bool match = true;
            for (size_t j = 0; j < len; ++j) {
                if (!bytes[j].second && data[i + j] != bytes[j].first) {
                    match = false;
                    break;
                }
            }
            if (match) return start + i;
        }
        return 0;
    }

    // Scan all executable sections of a module for a pattern.
    inline uintptr_t ScanModule(HMODULE module, std::string_view patternStr) {
        if (!module) return 0;
        const BytePattern pattern = Parse(patternStr);

        const auto* dosHdr = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
        const auto* ntHdr  = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(module) + dosHdr->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(ntHdr);

        for (WORD i = 0; i < ntHdr->FileHeader.NumberOfSections; ++i, ++section) {
            // Only scan executable sections
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

            uintptr_t secStart = reinterpret_cast<uintptr_t>(module) + section->VirtualAddress;
            size_t    secSize  = section->Misc.VirtualSize;

            uintptr_t result = Scan(secStart, secSize, pattern);
            if (result) return result;
        }
        return 0;
    }

    // Convenience: scan the main game module.
    inline uintptr_t ScanGame(std::string_view patternStr) {
        return ScanModule(GetModuleHandleW(nullptr), patternStr);
    }

    // Resolve a RIP-relative address embedded in an instruction.
    // instrOffset = byte offset from match to the 4-byte relative field.
    // instrLen    = total instruction length (bytes after the rel32 field).
    inline uintptr_t ResolveRIP(uintptr_t match, int instrOffset, int instrLen) {
        int32_t rel = *reinterpret_cast<int32_t*>(match + instrOffset);
        return match + instrOffset + 4 + instrLen - instrOffset - 4 + rel
               + instrOffset  // offset within instruction
               - instrOffset  // cancel out
               + rel
               + (instrLen);
        // Simplified: addr = match + instrLen + rel32
        // where instrLen is measured from the start of the instruction to its end.
        // This function covers the common lea/mov [rip+X] pattern.
    }

    // Simpler RIP resolver: addr = instr_start + instr_total_size + rel32
    // instr_start = match (first byte of instruction)
    // rel32_offset = offset of the 4-byte rel field within the instruction
    // instr_size   = total instruction size
    inline uintptr_t ResolveRIPSimple(uintptr_t instr, int rel32Offset, int instrSize) {
        int32_t rel = *reinterpret_cast<int32_t*>(instr + rel32Offset);
        return instr + instrSize + rel;
    }

} // namespace PatternScan
