#ifndef CUTILS_IO_STDOUT_WRITER_HPP
#define CUTILS_IO_STDOUT_WRITER_HPP

#include "cutils/io/concurrent_writer.hpp"
#include "cutils/io/fd_writer.hpp"

namespace cutils::io {

using StdoutWriter = ConcurrentWriter<BasicFdWriter<int>>;

}  // namespace cutils::io

#endif  // CUTILS_IO_STDOUT_WRITER_HPP
