#ifndef CUTILS_IO_STDIN_READER_HPP
#define CUTILS_IO_STDIN_READER_HPP

#include <unistd.h>

#include <cstddef>

#include "cutils/io/buffered_reader.hpp"
#include "cutils/io/fd_reader.hpp"

namespace cutils::io {

using StdinReader = BufferedReader<BasicFdReader<int>>;

inline constexpr std::size_t kStdinCapacity = 4096;

inline constinit StdinReader stdin_reader{kStdinCapacity, STDIN_FILENO};

}  // namespace cutils::io

#endif  // CUTILS_IO_STDIN_READER_HPP
