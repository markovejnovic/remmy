#ifndef CUTILS_IO_READER_HPP
#define CUTILS_IO_READER_HPP

#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <system_error>

namespace cutils::io {

template <class T>
concept Reader = requires(T& a, std::span<char> buf) {
  { a.Read(buf) } -> std::convertible_to<std::expected<std::size_t, std::errc>>;
};

}  // namespace cutils::io

#endif  // CUTILS_IO_READER_HPP
