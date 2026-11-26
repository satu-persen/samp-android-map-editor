#include "img_archive.h"

#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
#include <limits>
#include <new>
#include <exception>

namespace samp_editor {

#pragma pack(push, 1)
struct RawIMGv2Header {
    char magic[4];
    uint32_t entryCount;
};

struct RawIMGv2Entry {
    uint32_t offsetSector;
    uint16_t sizeSectors;
    uint16_t streamingSize;
    char name[24];
};
#pragma pack(pop)

namespace {

constexpr uint64_t kSectorSize = 2048ULL;
constexpr uint32_t kMaxEntryCount = 1000000U;

bool ReadExactFd(int fd, void* destination, size_t size, uint64_t offset) {
    auto* bytes = static_cast<uint8_t*>(destination);
    size_t total = 0;

    while (total < size) {
        const ssize_t count = pread(fd, bytes + total, size - total,
                                    static_cast<off_t>(offset + total));
        if (count <= 0) return false;
        total += static_cast<size_t>(count);
    }
    return true;
}

bool IsRangeInsideFile(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

} // namespace

IMGArchive::~IMGArchive() {
    Close();
}

void IMGArchive::Close() {
    if (m_FileStream.is_open()) {
        m_FileStream.close();
    }
    if (m_Fd >= 0) {
        close(m_Fd);
        m_Fd = -1;
    }
    m_Entries.clear();
    m_FilePath.clear();
    m_FileSize = 0;
}

bool IMGArchive::Open(const std::string& filePath) {
    Close();

    m_FilePath = filePath;
    m_FileStream.open(filePath, std::ios::binary);
    if (!m_FileStream.is_open()) {
        std::cerr << "[IMGArchive] Failed to open file: " << filePath << std::endl;
        Close();
        return false;
    }

    m_FileStream.seekg(0, std::ios::end);
    const std::streamoff end = m_FileStream.tellg();
    if (end < static_cast<std::streamoff>(sizeof(RawIMGv2Header))) {
        Close();
        return false;
    }

    m_FileSize = static_cast<uint64_t>(end);
    m_FileStream.seekg(0, std::ios::beg);

    RawIMGv2Header header{};
    m_FileStream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (m_FileStream.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header.magic, "VER2", 4) != 0) {
        std::cerr << "[IMGArchive] Invalid or incomplete IMG header" << std::endl;
        Close();
        return false;
    }

    if (!ReadDirectoryTable(header.entryCount)) {
        Close();
        return false;
    }
    return true;
}

bool IMGArchive::OpenFromFd(int fd, uint64_t fileLength) {
    Close();

    if (fd < 0) return false;

    m_Fd = dup(fd);
    if (m_Fd < 0) {
        std::cerr << "[IMGArchive] Failed to duplicate file descriptor" << std::endl;
        return false;
    }

    if (fileLength == 0) {
        struct stat info {};
        if (fstat(m_Fd, &info) != 0 || info.st_size <= 0) {
            std::cerr << "[IMGArchive] Could not determine IMG file size" << std::endl;
            Close();
            return false;
        }
        fileLength = static_cast<uint64_t>(info.st_size);
    }

    m_FileSize = fileLength;
    if (m_FileSize < sizeof(RawIMGv2Header)) {
        Close();
        return false;
    }

    RawIMGv2Header header{};
    if (!ReadExactFd(m_Fd, &header, sizeof(header), 0) ||
        std::memcmp(header.magic, "VER2", 4) != 0) {
        std::cerr << "[IMGArchive] Invalid or incomplete IMG header" << std::endl;
        Close();
        return false;
    }

    if (!ReadDirectoryTable(header.entryCount)) {
        Close();
        return false;
    }
    return true;
}

bool IMGArchive::ReadDirectoryTable(uint32_t entryCount) {
    m_Entries.clear();

    const uint64_t headerSize = sizeof(RawIMGv2Header);
    const uint64_t entrySize = sizeof(RawIMGv2Entry);

    if (entryCount == 0 || entryCount > kMaxEntryCount) {
        std::cerr << "[IMGArchive] Invalid directory entry count: "
                  << entryCount << std::endl;
        return false;
    }

    if (entryCount > (std::numeric_limits<uint64_t>::max() - headerSize) / entrySize) {
        return false;
    }

    const uint64_t tableSize64 = static_cast<uint64_t>(entryCount) * entrySize;
    if (!IsRangeInsideFile(headerSize, tableSize64, m_FileSize) ||
        tableSize64 > std::numeric_limits<size_t>::max()) {
        std::cerr << "[IMGArchive] Directory table exceeds file bounds" << std::endl;
        return false;
    }

    try {
        std::vector<RawIMGv2Entry> rawEntries(static_cast<size_t>(entryCount));

        if (m_FileStream.is_open()) {
            m_FileStream.clear();
            m_FileStream.seekg(static_cast<std::streamoff>(headerSize), std::ios::beg);
            m_FileStream.read(reinterpret_cast<char*>(rawEntries.data()),
                              static_cast<std::streamsize>(tableSize64));
            if (m_FileStream.gcount() != static_cast<std::streamsize>(tableSize64)) {
                std::cerr << "[IMGArchive] Failed to read complete directory table" << std::endl;
                return false;
            }
        } else if (m_Fd >= 0) {
            if (!ReadExactFd(m_Fd, rawEntries.data(),
                             static_cast<size_t>(tableSize64), headerSize)) {
                std::cerr << "[IMGArchive] Failed to read directory table from fd" << std::endl;
                return false;
            }
        } else {
            return false;
        }

        m_Entries.reserve(static_cast<size_t>(entryCount));

        for (uint32_t i = 0; i < entryCount; ++i) {
            const RawIMGv2Entry& raw = rawEntries[i];

            const uint64_t offsetBytes =
                static_cast<uint64_t>(raw.offsetSector) * kSectorSize;
            const uint64_t sizeBytes =
                static_cast<uint64_t>(raw.sizeSectors) * kSectorSize;

            if (sizeBytes > 0 && !IsRangeInsideFile(offsetBytes, sizeBytes, m_FileSize)) {
                std::cerr << "[IMGArchive] Skipping out-of-range entry at index "
                          << i << std::endl;
                continue;
            }

            char cleanName[25]{};
            std::memcpy(cleanName, raw.name, 24);
            const std::string name(cleanName);
            if (name.empty()) continue;

            IMGEntry entry;
            entry.offsetSector = raw.offsetSector;
            entry.sizeSectors = raw.sizeSectors;
            entry.streamingSize = raw.streamingSize;
            entry.name = name;

            m_Entries[ToLower(name)] = std::move(entry);
        }
    } catch (const std::bad_alloc&) {
        std::cerr << "[IMGArchive] Not enough memory to read directory table" << std::endl;
        m_Entries.clear();
        return false;
    } catch (const std::exception& error) {
        std::cerr << "[IMGArchive] Directory parsing failed: "
                  << error.what() << std::endl;
        m_Entries.clear();
        return false;
    }

    return !m_Entries.empty();
}

bool IMGArchive::HasEntry(const std::string& name) const {
    return m_Entries.find(ToLower(name)) != m_Entries.end();
}

const IMGEntry* IMGArchive::FindEntry(const std::string& name) const {
    const auto it = m_Entries.find(ToLower(name));
    return it != m_Entries.end() ? &it->second : nullptr;
}

bool IMGArchive::ReadEntry(const std::string& name, std::vector<uint8_t>& outBuffer) {
    const IMGEntry* entry = FindEntry(name);
    if (!entry) return false;
    return ReadEntry(*entry, outBuffer);
}

bool IMGArchive::ReadEntry(const IMGEntry& entry, std::vector<uint8_t>& outBuffer) {
    outBuffer.clear();

    const uint64_t offsetBytes = entry.GetByteOffset();
    const uint64_t sizeBytes = entry.GetByteSize();

    if (sizeBytes == 0 ||
        !IsRangeInsideFile(offsetBytes, sizeBytes, m_FileSize) ||
        sizeBytes > std::numeric_limits<size_t>::max()) {
        return false;
    }

    try {
        outBuffer.resize(static_cast<size_t>(sizeBytes));
    } catch (const std::bad_alloc&) {
        outBuffer.clear();
        std::cerr << "[IMGArchive] Not enough memory to read entry: "
                  << entry.name << std::endl;
        return false;
    }

    bool success = false;
    if (m_FileStream.is_open()) {
        m_FileStream.clear();
        m_FileStream.seekg(static_cast<std::streamoff>(offsetBytes), std::ios::beg);
        m_FileStream.read(reinterpret_cast<char*>(outBuffer.data()),
                          static_cast<std::streamsize>(sizeBytes));
        success = m_FileStream.gcount() == static_cast<std::streamsize>(sizeBytes);
    } else if (m_Fd >= 0) {
        success = ReadExactFd(m_Fd, outBuffer.data(),
                              static_cast<size_t>(sizeBytes), offsetBytes);
    }

    if (!success) outBuffer.clear();
    return success;
}

} // namespace samp_editor
