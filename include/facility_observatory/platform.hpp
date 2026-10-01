// Facility Observatory - durable file and lock primitives.
//
// These are deliberately thin, explicit and platform-honest: a commit point is
// a real fsync/FlushFileBuffers, and the single-writer lock is enforced by the
// kernel rather than by convention.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_PLATFORM_HPP
#define FACILITY_OBSERVATORY_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"

namespace fo {

// Absolute, lexically normalised path. Does not touch the file system. Paths
// longer than the classic platform limit are handled by the primitives below.
FO_API Outcome<std::string> normalize_path(const std::string& path);

FO_API bool path_exists(const std::string& path);
FO_API Outcome<std::uint64_t> file_size(const std::string& path);

// ---------------------------------------------------------------------------
// DurableFile
//
// append() writes but does not make anything durable; commit() is the single
// explicit commit point. A process that dies before commit() leaves a torn tail
// that recovery is required to detect.
// ---------------------------------------------------------------------------
class FO_API DurableFile {
 public:
  DurableFile() = default;
  ~DurableFile();
  DurableFile(const DurableFile&) = delete;
  DurableFile& operator=(const DurableFile&) = delete;
  DurableFile(DurableFile&& other) noexcept;
  DurableFile& operator=(DurableFile&& other) noexcept;

  static Outcome<DurableFile> open_append(const std::string& path);
  static Outcome<DurableFile> open_read(const std::string& path);

  [[nodiscard]] bool is_open() const noexcept { return handle_ >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  [[nodiscard]] Outcome<std::uint64_t> size() const;
  [[nodiscard]] Outcome<std::size_t> read_at(std::uint64_t offset, void* buffer, std::size_t count) const;

  Outcome<Nothing> append(const void* data, std::size_t count);
  Outcome<Nothing> commit();
  Outcome<Nothing> truncate(std::uint64_t size);
  // Best effort: makes a directory entry durable where the platform supports it.
  Outcome<Nothing> sync_containing_directory() const;

  void close() noexcept;

 private:
  std::intptr_t handle_{-1};
  std::string path_{};
  bool writable_{false};
};

// ---------------------------------------------------------------------------
// FileLock
//
// An exclusive advisory lock held by the kernel for as long as the handle is
// open. A second acquisition -- in another process, or in this process through a
// second handle -- fails immediately rather than blocking.
// ---------------------------------------------------------------------------
class FO_API FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  static Outcome<FileLock> acquire_exclusive(const std::string& path);

  [[nodiscard]] bool held() const noexcept { return handle_ >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  void release() noexcept;

 private:
  std::intptr_t handle_{-1};
  std::string path_{};
};

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

// Writes to a sibling temporary file, commits it, then renames it over the
// destination. A reader never observes a partially written destination.
FO_API Status publish_atomically(const std::string& destination, const void* data, std::size_t count);

FO_API Outcome<std::vector<std::uint8_t>> read_whole_file(const std::string& path, std::size_t max_bytes);

FO_API Status remove_file(const std::string& path) noexcept;

// A short, process-unique suffix used for temporary file names.
FO_API std::string unique_suffix();

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_PLATFORM_HPP
