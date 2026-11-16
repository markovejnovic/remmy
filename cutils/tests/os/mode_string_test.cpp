// See LICENSE in the repository root.

#include <sys/stat.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cutils/os/mode_string.hpp>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <string.h>  // strmode(3)

TEST_CASE("ModeString matches libc strmode for every mode", "[mode_string]") {
  std::size_t mismatches = 0;
  for (unsigned mode = 0; mode <= 0xFFFFU; ++mode) {
    std::array<char, 12> libc{};
    ::strmode(static_cast<mode_t>(mode), libc.data());
    const std::string_view expected{libc.data()};
    if (cutils::os::ModeString(static_cast<mode_t>(mode)).View() != expected) {
      ++mismatches;
      UNSCOPED_INFO("mode "
                    << mode << ": libc '" << expected << "', ours '"
                    << cutils::os::ModeString(static_cast<mode_t>(mode)).View()
                    << "'");
    }
  }
  CHECK(mismatches == 0);
}
#endif

TEST_CASE("ModeString renders type, permissions and the special bits",
          "[mode_string]") {
  const auto [mode, text] = GENERATE(
      std::pair<mode_t, std::string_view>{S_IFREG | 0644, "-rw-r--r-- "},
      std::pair<mode_t, std::string_view>{S_IFDIR | 0755, "drwxr-xr-x "},
      std::pair<mode_t, std::string_view>{S_IFLNK | 0777, "lrwxrwxrwx "},
      std::pair<mode_t, std::string_view>{S_IFREG | 04444, "-r-Sr--r-- "},
      std::pair<mode_t, std::string_view>{S_IFREG | 06755, "-rwsr-sr-x "},
      std::pair<mode_t, std::string_view>{S_IFDIR | 01777, "drwxrwxrwt "},
      std::pair<mode_t, std::string_view>{S_IFREG | 01444, "-r--r--r-T "},
      std::pair<mode_t, std::string_view>{S_IFIFO | 0600, "prw------- "},
      std::pair<mode_t, std::string_view>{S_IFSOCK | 0, "s--------- "});
  CAPTURE(mode);
  CHECK(cutils::os::ModeString(mode).View() == text);
}
