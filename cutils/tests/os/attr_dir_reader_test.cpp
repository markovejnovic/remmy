// See LICENSE in the repository root.

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <cstdlib>
#include <cutils/os/attr_dir_reader.hpp>
#include <cutils/os/os.hpp>
#include <filesystem>
#include <map>
#include <string>

namespace {

/// What the reader is expected to say about one entry.
struct Seen {
  cutils::os::EntryKind kind;
  std::uint32_t flags;
  bool writable;

  auto operator==(const Seen&) const -> bool = default;
};

/// A scratch directory holding one entry of each shape the override prompt
/// cares about, removed (flags cleared first) on destruction.
class Fixture {
 public:
  Fixture() {
    char name[] = "/tmp/attr_dir_reader.XXXXXX";
    REQUIRE(::mkdtemp(name) != nullptr);
    root_ = name;
    Touch("plain", 0644);
    Touch("readonly", 0444);
    REQUIRE(::symlink("readonly", (root_ / "link").c_str()) == 0);
    REQUIRE(::mkdir((root_ / "subdir").c_str(), 0755) == 0);
#if defined(__APPLE__)
    Touch("immutable", 0644);
    REQUIRE(::chflags((root_ / "immutable").c_str(), UF_IMMUTABLE) == 0);
    Touch("denied", 0644);
    const ::passwd* me = ::getpwuid(::getuid());
    REQUIRE(me != nullptr);
    const std::string deny = "/bin/chmod +a \"" + std::string{me->pw_name} +
                             " deny write\" " + (root_ / "denied").string();
    REQUIRE(std::system(deny.c_str()) == 0);
#endif
  }

  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;

  ~Fixture() {
#if defined(__APPLE__)
    ::chflags((root_ / "immutable").c_str(), 0);
#endif
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  [[nodiscard]] auto Root() const -> const std::filesystem::path& {
    return root_;
  }

  /// What fstatat(2) and access(2) say about every entry.
  [[nodiscard]] auto Expected() const -> std::map<std::string, Seen> {
    std::map<std::string, Seen> out;
    for (const auto& entry : std::filesystem::directory_iterator(root_)) {
      struct stat st;
      REQUIRE(::lstat(entry.path().c_str(), &st) == 0);
      const auto kind = S_ISDIR(st.st_mode) ? cutils::os::EntryKind::kDirectory
                        : S_ISLNK(st.st_mode) ? cutils::os::EntryKind::kSymlink
                                              : cutils::os::EntryKind::kOther;
#if defined(__APPLE__)
      const std::uint32_t flags = st.st_flags;
#else
      const std::uint32_t flags = 0;
#endif
      // Symlinks are never asked about, so their writability is not compared.
      const bool writable = kind != cutils::os::EntryKind::kSymlink &&
                            ::access(entry.path().c_str(), W_OK) == 0;
      out[entry.path().filename().string()] = Seen{kind, flags, writable};
    }
    return out;
  }

 private:
  void Touch(const char* name, mode_t mode) const {
    const int fd = ::open((root_ / name).c_str(), O_CREAT | O_WRONLY, mode);
    REQUIRE(fd >= 0);
    ::close(fd);
    REQUIRE(::chmod((root_ / name).c_str(), mode) == 0);
  }

  std::filesystem::path root_;
};

}  // namespace

TEST_CASE("Each entry's kind, flags and writability match fstatat and access",
          "[attr_dir_reader]") {
  using Method = cutils::os::AttrDirReader::Method;
  const Method method = GENERATE(Method::kBulk, Method::kPerEntry);
  CAPTURE(method == Method::kBulk);

  const Fixture fixture;
  auto dir = cutils::os::open(fixture.Root().c_str(), O_RDONLY | O_DIRECTORY);
  REQUIRE(dir);

  std::map<std::string, Seen> seen;
  cutils::os::AttrDirReader reader(method);
  const auto read = reader.ForEach(*dir, [&](const cutils::os::AttrEntry& e) {
    CHECK(e.error() == 0);
    const bool writable =
        e.kind() != cutils::os::EntryKind::kSymlink && e.writable();
    seen[std::string{e.name()}] = Seen{e.kind(), e.flags(), writable};
  });

  REQUIRE(read);
  CHECK(seen == fixture.Expected());
}
