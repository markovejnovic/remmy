#ifndef CUTILS_IO_WRITER_REF_HPP
#define CUTILS_IO_WRITER_REF_HPP

#include <cstddef>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

#include "cutils/io/writer.hpp"

namespace cutils::io {

template <Writer W>
class WriterRef {
 public:
  explicit constexpr WriterRef(W& writer) noexcept : writer_(&writer) {}

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return writer_->Write(sv);
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    return writer_->WriteMany(std::forward<R>(pieces));
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    return writer_->Flush();
  }

 private:
  W* writer_;
};

}  // namespace cutils::io

#endif  // CUTILS_IO_WRITER_REF_HPP
