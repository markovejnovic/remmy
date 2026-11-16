// See LICENSE in the repository root.

#ifndef CUTILS_IO_STRING_WRITER_HPP
#define CUTILS_IO_STRING_WRITER_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include "cutils/io/writer.hpp"

namespace cutils::io {

/// @brief Writer that appends to a borrowed std::string.
class StringWriter {
 public:
  explicit StringWriter(std::string& out) noexcept : out_(&out) {}

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    out_->append(sv);
    return sv.size();
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    for (const std::string_view piece : pieces) {
      out_->append(piece);
    }
    return {};
  }

  [[nodiscard]] static auto Flush() noexcept -> std::expected<void, std::errc> {
    return {};
  }

 private:
  std::string* out_;
};

}  // namespace cutils::io

#endif  // CUTILS_IO_STRING_WRITER_HPP
