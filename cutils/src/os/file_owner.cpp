// See LICENSE in the repository root.

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "cutils/os/file_flags.hpp"
#include "cutils/os/owner_names.hpp"

#if defined(__APPLE__)
#include <grp.h>
#include <pwd.h>

#include <cstdlib>
#include <memory>
#else
#include <fcntl.h>

#include <array>
#include <charconv>
#include <optional>
#include <ranges>
#include <string_view>
#endif

namespace cutils::os {

#if defined(__APPLE__)

auto FlagNames(std::uint32_t flags) -> std::string {
  const std::unique_ptr<char, decltype(&std::free)> names(::fflagstostr(flags),
                                                          &std::free);
  // fflagstostr fails only for want of memory; print no flags then.
  return names != nullptr ? std::string{names.get()} : std::string{};
}

auto UserName(uid_t uid) -> std::string { return ::user_from_uid(uid, 0); }

auto GroupName(gid_t gid) -> std::string { return ::group_from_gid(gid, 0); }

#else

namespace {

/// @brief The whole of the file at `path`, or empty if it cannot be read.
auto ReadAll(const char* path) -> std::string {
  std::string text;
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return text;
  }
  std::array<char, 4096> chunk;
  ssize_t got = 0;
  while ((got = ::read(fd, chunk.data(), chunk.size())) > 0) {
    text.append(chunk.data(), static_cast<std::size_t>(got));
  }
  ::close(fd);
  return text;
}

/// @brief The name in the first line of a passwd(5) or group(5) file whose
///        third field is `id`, if any.
auto NameFor(const char* path, std::uint32_t id) -> std::optional<std::string> {
  const std::string text = ReadAll(path);
  for (const auto line : std::views::split(std::string_view{text}, '\n')) {
    auto fields = std::views::split(std::string_view{line}, ':');
    auto field = fields.begin();
    if (field == fields.end()) {
      continue;
    }
    const std::string_view name{*field};
    if (++field == fields.end() || ++field == fields.end()) {
      continue;
    }
    const std::string_view number{*field};
    std::uint32_t value = 0;
    const auto [end, error] =
        std::from_chars(number.data(), number.data() + number.size(), value);
    if (error == std::errc{} && end == number.data() + number.size() &&
        value == id) {
      return std::string{name};
    }
  }
  return std::nullopt;
}

}  // namespace

auto FlagNames(std::uint32_t /*flags*/) -> std::string { return {}; }

auto UserName(uid_t uid) -> std::string {
  return NameFor("/etc/passwd", uid).value_or(std::to_string(uid));
}

auto GroupName(gid_t gid) -> std::string {
  return NameFor("/etc/group", gid).value_or(std::to_string(gid));
}

#endif

}  // namespace cutils::os
