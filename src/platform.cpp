// Facility Observatory - durable file and lock primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/platform.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fo {
namespace {

std::string describe_system_error() {
#if defined(_WIN32)
  const DWORD code = ::GetLastError();
  return "windows error " + std::to_string(static_cast<unsigned long>(code));
#else
  return "errno " + std::to_string(errno);
#endif
}

#if defined(_WIN32)
std::wstring widen(std::string_view text) {
  if (text.empty()) {
    return std::wstring{};
  }
  const int length = static_cast<int>(text.size());
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
  if (needed <= 0) {
    return std::wstring{};
  }
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), needed) <= 0) {
    return std::wstring{};
  }
  return result;
}

// Windows refuses paths beyond MAX_PATH unless they are expressed in the
// extended-length form, which also requires a fully qualified path with
// backslash separators and no relative components.
std::wstring to_extended_path(const std::wstring& absolute) {
  if (absolute.rfind(L"\\\\?\\", 0) == 0) {
    return absolute;
  }
  if (absolute.size() < 240) {
    return absolute;
  }
  if (absolute.rfind(L"\\\\", 0) == 0) {
    return L"\\\\?\\UNC\\" + absolute.substr(2);
  }
  return L"\\\\?\\" + absolute;
}

Outcome<std::wstring> native_path(const std::string& path) {
  std::wstring wide = widen(path);
  if (wide.empty()) {
    return Outcome<std::wstring>::fail(ReasonCode::journal_path_invalid, "path is empty or not valid UTF-8");
  }
  for (wchar_t& c : wide) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  if (wide.size() >= 2 && wide[1] == L':' ) {
    // Drive-qualified already.
  } else if (wide.rfind(L"\\\\", 0) != 0) {
    return Outcome<std::wstring>::fail(ReasonCode::journal_path_invalid,
                                       "path must be absolute before it can be opened");
  }
  return Outcome<std::wstring>::ok(to_extended_path(wide));
}
#endif

}  // namespace

Outcome<std::string> normalize_path(const std::string& path) {
  if (path.empty()) {
    return Outcome<std::string>::fail(ReasonCode::journal_path_invalid, "path is empty");
  }
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(path), error);
  if (error) {
    return Outcome<std::string>::fail(ReasonCode::journal_path_invalid,
                                      "cannot make '" + path + "' absolute: " + error.message());
  }
  std::filesystem::path normalized = absolute.lexically_normal();
  if (normalized.empty()) {
    return Outcome<std::string>::fail(ReasonCode::journal_path_invalid, "path '" + path + "' normalises to nothing");
  }
  return Outcome<std::string>::ok(normalized.string());
}

bool path_exists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(std::filesystem::path(path), error) && !error;
}

Outcome<std::uint64_t> file_size(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
  if (error) {
    return Outcome<std::uint64_t>::fail(ReasonCode::journal_missing,
                                        "cannot size '" + path + "': " + error.message());
  }
  return Outcome<std::uint64_t>::ok(static_cast<std::uint64_t>(size));
}

DurableFile::~DurableFile() { close(); }

DurableFile::DurableFile(DurableFile&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)), writable_(other.writable_) {
  other.handle_ = -1;
  other.writable_ = false;
  other.path_.clear();
}

DurableFile& DurableFile::operator=(DurableFile&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    writable_ = other.writable_;
    other.handle_ = -1;
    other.writable_ = false;
    other.path_.clear();
  }
  return *this;
}

Outcome<DurableFile> DurableFile::open_append(const std::string& path) {
  auto normalized = normalize_path(path);
  if (!normalized) {
    return normalized.propagate<DurableFile>();
  }
  DurableFile file;
  file.path_ = normalized.value();
  file.writable_ = true;
#if defined(_WIN32)
  auto native = native_path(file.path_);
  if (!native) {
    return native.propagate<DurableFile>();
  }
  const HANDLE handle = ::CreateFileW(native.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Outcome<DurableFile>::fail(ReasonCode::journal_io_error,
                                      "cannot open '" + file.path_ + "' for append: " + describe_system_error());
  }
  file.handle_ = reinterpret_cast<std::intptr_t>(handle);
  LARGE_INTEGER end{};
  if (::SetFilePointerEx(handle, LARGE_INTEGER{}, nullptr, FILE_END) == 0) {
    const std::string detail = describe_system_error();
    file.close();
    return Outcome<DurableFile>::fail(ReasonCode::journal_io_error,
                                      "cannot seek to end of '" + path + "': " + detail);
  }
  (void)end;
#else
  const int descriptor = ::open(file.path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return Outcome<DurableFile>::fail(ReasonCode::journal_io_error,
                                      "cannot open '" + file.path_ + "' for append: " + describe_system_error());
  }
  file.handle_ = static_cast<std::intptr_t>(descriptor);
  if (::lseek(descriptor, 0, SEEK_END) < 0) {
    const std::string detail = describe_system_error();
    file.close();
    return Outcome<DurableFile>::fail(ReasonCode::journal_io_error,
                                      "cannot seek to end of '" + path + "': " + detail);
  }
#endif
  return Outcome<DurableFile>::ok(std::move(file));
}

Outcome<DurableFile> DurableFile::open_read(const std::string& path) {
  auto normalized = normalize_path(path);
  if (!normalized) {
    return normalized.propagate<DurableFile>();
  }
  DurableFile file;
  file.path_ = normalized.value();
  file.writable_ = false;
#if defined(_WIN32)
  auto native = native_path(file.path_);
  if (!native) {
    return native.propagate<DurableFile>();
  }
  const HANDLE handle = ::CreateFileW(native.value().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    const ReasonCode reason =
        (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? ReasonCode::journal_missing
                                                                     : ReasonCode::journal_io_error;
    return Outcome<DurableFile>::fail(reason,
                                      "cannot open '" + file.path_ + "' for reading: " + describe_system_error());
  }
  file.handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
  const int descriptor = ::open(file.path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    const ReasonCode reason = (errno == ENOENT) ? ReasonCode::journal_missing : ReasonCode::journal_io_error;
    return Outcome<DurableFile>::fail(reason,
                                      "cannot open '" + file.path_ + "' for reading: " + describe_system_error());
  }
  file.handle_ = static_cast<std::intptr_t>(descriptor);
#endif
  return Outcome<DurableFile>::ok(std::move(file));
}

void DurableFile::close() noexcept {
  if (handle_ < 0) {
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = -1;
}

Outcome<std::uint64_t> DurableFile::size() const {
  if (!is_open()) {
    return Outcome<std::uint64_t>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER length{};
  if (::GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &length) == 0) {
    return Outcome<std::uint64_t>::fail(ReasonCode::journal_io_error,
                                        "cannot size '" + path_ + "': " + describe_system_error());
  }
  return Outcome<std::uint64_t>::ok(static_cast<std::uint64_t>(length.QuadPart));
#else
  struct stat info {};
  if (::fstat(static_cast<int>(handle_), &info) != 0) {
    return Outcome<std::uint64_t>::fail(ReasonCode::journal_io_error,
                                        "cannot size '" + path_ + "': " + describe_system_error());
  }
  return Outcome<std::uint64_t>::ok(static_cast<std::uint64_t>(info.st_size));
#endif
}

Outcome<std::size_t> DurableFile::read_at(std::uint64_t offset, void* buffer, std::size_t count) const {
  if (!is_open()) {
    return Outcome<std::size_t>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
  if (count == 0) {
    return Outcome<std::size_t>::ok(0);
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (::SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) == 0) {
    return Outcome<std::size_t>::fail(ReasonCode::journal_io_error,
                                      "cannot seek in '" + path_ + "': " + describe_system_error());
  }
  DWORD read = 0;
  const DWORD wanted = static_cast<DWORD>(count);
  if (::ReadFile(reinterpret_cast<HANDLE>(handle_), buffer, wanted, &read, nullptr) == 0) {
    return Outcome<std::size_t>::fail(ReasonCode::journal_io_error,
                                      "cannot read '" + path_ + "': " + describe_system_error());
  }
  return Outcome<std::size_t>::ok(static_cast<std::size_t>(read));
#else
  const ssize_t read = ::pread(static_cast<int>(handle_), buffer, count, static_cast<off_t>(offset));
  if (read < 0) {
    return Outcome<std::size_t>::fail(ReasonCode::journal_io_error,
                                      "cannot read '" + path_ + "': " + describe_system_error());
  }
  return Outcome<std::size_t>::ok(static_cast<std::size_t>(read));
#endif
}

Outcome<Nothing> DurableFile::append(const void* data, std::size_t count) {
  if (!is_open()) {
    return Outcome<Nothing>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
  if (!writable_) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error, "file was opened read-only");
  }
  if (count == 0) {
    return success();
  }
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < count) {
#if defined(_WIN32)
    const DWORD chunk = static_cast<DWORD>(count - written);
    DWORD done = 0;
    if (::WriteFile(reinterpret_cast<HANDLE>(handle_), cursor + written, chunk, &done, nullptr) == 0) {
      return Outcome<Nothing>::fail(ReasonCode::journal_commit_failed,
                                    "cannot write to '" + path_ + "': " + describe_system_error());
    }
    if (done == 0) {
      return Outcome<Nothing>::fail(ReasonCode::journal_commit_failed,
                                    "write to '" + path_ + "' made no progress");
    }
    written += static_cast<std::size_t>(done);
#else
    const ssize_t done = ::write(static_cast<int>(handle_), cursor + written, count - written);
    if (done < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Outcome<Nothing>::fail(ReasonCode::journal_commit_failed,
                                    "cannot write to '" + path_ + "': " + describe_system_error());
    }
    written += static_cast<std::size_t>(done);
#endif
  }
  return success();
}

Outcome<Nothing> DurableFile::commit() {
  if (!is_open()) {
    return Outcome<Nothing>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
  if (!writable_) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error, "file was opened read-only");
  }
#if defined(_WIN32)
  if (::FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)) == 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_commit_failed,
                                  "cannot flush '" + path_ + "': " + describe_system_error());
  }
#else
  if (::fsync(static_cast<int>(handle_)) != 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_commit_failed,
                                  "cannot fsync '" + path_ + "': " + describe_system_error());
  }
#endif
  return success();
}

Outcome<Nothing> DurableFile::truncate(std::uint64_t size) {
  if (!is_open()) {
    return Outcome<Nothing>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
  if (!writable_) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error, "file was opened read-only");
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) == 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error,
                                  "cannot seek in '" + path_ + "': " + describe_system_error());
  }
  if (::SetEndOfFile(reinterpret_cast<HANDLE>(handle_)) == 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error,
                                  "cannot truncate '" + path_ + "': " + describe_system_error());
  }
#else
  if (::ftruncate(static_cast<int>(handle_), static_cast<off_t>(size)) != 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error,
                                  "cannot truncate '" + path_ + "': " + describe_system_error());
  }
#endif
  return success();
}

Outcome<Nothing> DurableFile::sync_containing_directory() const {
  if (!is_open()) {
    return Outcome<Nothing>::fail(ReasonCode::journal_not_open, "file handle is not open");
  }
#if defined(_WIN32)
  // NTFS metadata ordering already makes a completed rename durable; there is no
  // directory handle to flush through the Win32 API.
  return success();
#else
  std::error_code error;
  const std::filesystem::path parent = std::filesystem::path(path_).parent_path();
  if (parent.empty()) {
    return success();
  }
  const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error,
                                  "cannot open directory '" + parent.string() + "': " + describe_system_error());
  }
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  if (result != 0) {
    return Outcome<Nothing>::fail(ReasonCode::journal_io_error,
                                  "cannot fsync directory '" + parent.string() + "': " + describe_system_error());
  }
  return success();
#endif
}

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = -1;
  other.path_.clear();
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = -1;
    other.path_.clear();
  }
  return *this;
}

Outcome<FileLock> FileLock::acquire_exclusive(const std::string& path) {
  auto normalized = normalize_path(path);
  if (!normalized) {
    return normalized.propagate<FileLock>();
  }
  FileLock lock;
  lock.path_ = normalized.value();
#if defined(_WIN32)
  auto native = native_path(lock.path_);
  if (!native) {
    return native.propagate<FileLock>();
  }
  const HANDLE handle = ::CreateFileW(native.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Outcome<FileLock>::fail(ReasonCode::journal_io_error,
                                   "cannot open lock file '" + lock.path_ + "': " + describe_system_error());
  }
  OVERLAPPED overlapped{};
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                   &overlapped) == 0) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(handle);
    const ReasonCode reason =
        (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) ? ReasonCode::journal_locked
                                                                   : ReasonCode::lock_unavailable;
    return Outcome<FileLock>::fail(
        reason, "another writer already holds '" + lock.path_ + "' (windows error " +
                    std::to_string(static_cast<unsigned long>(code)) + ")");
  }
  lock.handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
  const int descriptor = ::open(lock.path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return Outcome<FileLock>::fail(ReasonCode::journal_io_error,
                                   "cannot open lock file '" + lock.path_ + "': " + describe_system_error());
  }
  struct flock request {};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 0;
  request.l_len = 0;
  if (::fcntl(descriptor, F_SETLK, &request) != 0) {
    const int code = errno;
    ::close(descriptor);
    const ReasonCode reason =
        (code == EACCES || code == EAGAIN) ? ReasonCode::journal_locked : ReasonCode::lock_unavailable;
    return Outcome<FileLock>::fail(reason, "another writer already holds '" + lock.path_ + "'");
  }
  lock.handle_ = static_cast<std::intptr_t>(descriptor);
#endif
  return Outcome<FileLock>::ok(std::move(lock));
}

void FileLock::release() noexcept {
  if (handle_ < 0) {
    return;
  }
#if defined(_WIN32)
  HANDLE handle = reinterpret_cast<HANDLE>(handle_);
  OVERLAPPED overlapped{};
  ::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
  ::CloseHandle(handle);
#else
  struct flock request {};
  request.l_type = F_UNLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 0;
  request.l_len = 0;
  ::fcntl(static_cast<int>(handle_), F_SETLK, &request);
  ::close(static_cast<int>(handle_));
#endif
  handle_ = -1;
}

Status publish_atomically(const std::string& destination, const void* data, std::size_t count) {
  auto normalized = normalize_path(destination);
  if (!normalized) {
    return normalized.propagate<Nothing>();
  }
  const std::string temporary = normalized.value() + ".tmp." + unique_suffix();
  {
    auto file = DurableFile::open_append(temporary);
    if (!file) {
      return file.propagate<Nothing>();
    }
    if (count > 0) {
      auto written = file.value().append(data, count);
      if (!written) {
        return written;
      }
    }
    auto committed = file.value().commit();
    if (!committed) {
      return committed;
    }
  }
  std::error_code error;
  std::filesystem::rename(std::filesystem::path(temporary), std::filesystem::path(normalized.value()), error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path(temporary), ignored);
    return Status::fail(ReasonCode::journal_io_error,
                        "cannot publish '" + normalized.value() + "': " + error.message());
  }
  return success();
}

Outcome<std::vector<std::uint8_t>> read_whole_file(const std::string& path, std::size_t max_bytes) {
  auto file = DurableFile::open_read(path);
  if (!file) {
    return file.propagate<std::vector<std::uint8_t>>();
  }
  auto length = file.value().size();
  if (!length) {
    return length.propagate<std::vector<std::uint8_t>>();
  }
  if (length.value() > max_bytes) {
    return Outcome<std::vector<std::uint8_t>>::fail(
        ReasonCode::record_too_large,
        "file '" + path + "' is " + std::to_string(length.value()) + " bytes, beyond the bound of " +
            std::to_string(max_bytes) + " bytes");
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length.value()));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    auto read = file.value().read_at(offset, buffer.data() + offset, buffer.size() - offset);
    if (!read) {
      return read.propagate<std::vector<std::uint8_t>>();
    }
    if (read.value() == 0) {
      return Outcome<std::vector<std::uint8_t>>::fail(ReasonCode::journal_io_error,
                                                      "file '" + path + "' ended early during read");
    }
    offset += read.value();
  }
  return Outcome<std::vector<std::uint8_t>>::ok(std::move(buffer));
}

Status remove_file(const std::string& path) noexcept {
  std::error_code error;
  std::filesystem::remove(std::filesystem::path(path), error);
  if (error) {
    return Status::fail(ReasonCode::journal_io_error, "cannot remove '" + path + "': " + error.message());
  }
  return success();
}

std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(static_cast<unsigned long long>(value));
}

}  // namespace fo
