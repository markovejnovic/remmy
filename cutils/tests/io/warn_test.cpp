// See LICENSE in the repository root.

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cutils/io/warn.hpp>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct CapturingWriter {
  auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    out.append(sv);
    ++calls;
    return sv.size();
  }

  template <cutils::io::PieceRange R>
  auto WriteMany(R&& pieces) noexcept -> std::expected<void, std::errc> {
    for (const std::string_view piece : pieces) {
      out.append(piece);
    }
    ++calls;
    return {};
  }

  static auto Flush() noexcept -> std::expected<void, std::errc> { return {}; }

  std::string out;
  std::size_t calls = 0;
};

auto Expected(std::string_view prog, std::string_view body, int error)
    -> std::string {
  return std::string(prog) + ": " + std::string(body) + ": " +
         std::strerror(error) + "\n";
}

}  // namespace

TEST_CASE("Warn prints prog, body and strerror in one write", "[warn]") {
  CapturingWriter w;
  REQUIRE(cutils::io::Warn(w, "rm", ENOENT, "{}", "a/b"));
  CHECK(w.out == Expected("rm", "a/b", ENOENT));
  CHECK(w.calls == 1);
}

TEST_CASE("Warnx prints prog and body only", "[warn]") {
  CapturingWriter w;
  REQUIRE(cutils::io::Warnx(w, "rm", "{}: is a directory", "d"));
  CHECK(w.out == "rm: d: is a directory\n");
  CHECK(w.calls == 1);
}

TEST_CASE("Warn escapes C0 control bytes like Apple's err(3)", "[warn]") {
  CapturingWriter w;
  REQUIRE(cutils::io::Warn(w, "rm", ENOENT, "{}",
                           std::string_view("a\001\t\n\r\033\177\x80z")));
  CHECK(w.out == Expected("rm", "a\\001\t\n\\r\\033\177\x80z", ENOENT));
}

TEST_CASE("Warn uses Apple's escape for every C0 byte", "[warn]") {
  static constexpr std::array<std::string_view, 32> kApple{
      "",      "\\001", "\\002", "\\003", "\\004", "\\005", "\\006", "\\a",
      "\\b",   "\t",    "\n",    "\\v",   "\\f",   "\\r",   "\\016", "\\017",
      "\\020", "\\021", "\\022", "\\023", "\\024", "\\025", "\\026", "\\027",
      "\\030", "\\031", "\\032", "\\033", "\\034", "\\035", "\\036", "\\037",
  };
  for (std::size_t byte = 1; byte < kApple.size(); ++byte) {
    const char c = static_cast<char>(byte);
    CapturingWriter w;
    REQUIRE(cutils::io::Warnx(w, "rm", "{}", std::string_view(&c, 1)));
    CHECK(w.out == "rm: " + std::string(kApple[byte]) + "\n");
  }
}

TEST_CASE("Warn leaves the program name and strerror raw", "[warn]") {
  CapturingWriter w;
  REQUIRE(cutils::io::Warn(w, "r\001m", EACCES, "{}", "\001"));
  CHECK(w.out == Expected("r\001m", "\\001", EACCES));
}

TEST_CASE("Warn escapes across pieces and skips empty ones", "[warn]") {
  CapturingWriter w;
  REQUIRE(cutils::io::Warn(w, "rm", ENOTDIR, "{}/{}{}", "\033", "",
                           std::string_view("x\001")));
  CHECK(w.out == Expected("rm", "\\033/x\\001", ENOTDIR));
}

TEST_CASE("Warn passes a path with nothing to escape through whole", "[warn]") {
  const std::string_view path = "plain/path";
  const std::array<std::string_view, 1> body{path};
  const cutils::io::detail::WarnPieces pieces({}, body, {});
  auto it = pieces.begin();
  REQUIRE(it != pieces.end());
  CHECK((*it).data() == path.data());
  CHECK((*it).size() == path.size());
  CHECK(++it == pieces.end());
}
