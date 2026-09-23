// See LICENSE in the repository root.

#include <fcntl.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cutils/os/fd.hpp>
#include <cutils/os/limits/fd.hpp>
#include <optional>
#include <type_traits>
#include <utility>

namespace {
using cutils::os::Fd;

static_assert(sizeof(Fd) == sizeof(int));
static_assert(sizeof(std::optional<Fd>) == sizeof(Fd));

auto OpenRaw() -> int {
  const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  return fd;
}
auto OpenNull() -> Fd { return Fd{OpenRaw()}; }

auto IsOpen(int fd) -> bool { return ::fcntl(fd, F_GETFD) != -1; }
auto IsClosed(int fd) -> bool {
  errno = 0;
  const int result = ::fcntl(fd, F_GETFD);
  return result == -1 && errno == EBADF;
}

SCENARIO("A default Fd owns no descriptor", "[fd]") {
  GIVEN("a default-constructed Fd") {
    const Fd empty;
    THEN("it is closed and holds the invalid descriptor") {
      REQUIRE(!empty.IsOpen());
      REQUIRE(empty.get() == Fd::kInvalid);
    }
  }
}

SCENARIO("Fd closes its descriptor on destruction", "[fd]") {
  GIVEN("an owned descriptor") {
    int watched = Fd::kInvalid;
    WHEN("its owner leaves scope") {
      {
        const Fd fd = OpenNull();
        REQUIRE(fd.IsOpen());
        watched = fd.get();
        REQUIRE(IsOpen(watched));
      }
      THEN("the descriptor is closed") { REQUIRE(IsClosed(watched)); }
    }
  }
}

SCENARIO("Moving an Fd transfers ownership", "[fd]") {
  GIVEN("an Fd owning an open descriptor") {
    Fd source = OpenNull();
    const int watched = source.get();

    WHEN("another Fd is move-constructed from it") {
      Fd moved = std::move(source);
      THEN("the new owner holds the open descriptor and the source is empty") {
        REQUIRE(moved.get() == watched);
        REQUIRE(!source.IsOpen());
        REQUIRE(IsOpen(watched));
      }

      AND_WHEN("it is move-assigned to an Fd that already owns a descriptor") {
        Fd target = OpenNull();
        const int replaced = target.get();
        target = std::move(moved);
        THEN("the old descriptor closes and ownership transfers again") {
          REQUIRE(IsClosed(replaced));
          REQUIRE(target.get() == watched);
          REQUIRE(!moved.IsOpen());
          REQUIRE(IsOpen(watched));
        }
      }
    }
  }
}

SCENARIO("Self-move assignment preserves an Fd", "[fd]") {
  GIVEN("an Fd owning an open descriptor") {
    Fd fd = OpenNull();
    const int watched = fd.get();
    WHEN("it is move-assigned through an alias of itself") {
      auto& alias = fd;
      fd = std::move(alias);
      THEN("it still owns the same open descriptor") {
        REQUIRE(fd.get() == watched);
        REQUIRE(IsOpen(watched));
      }
    }
  }
}

SCENARIO("Releasing an Fd transfers responsibility to the caller", "[fd]") {
  GIVEN("an Fd owning an open descriptor") {
    Fd fd = OpenNull();
    const int watched = fd.get();
    WHEN("ownership is released") {
      const int released = fd.Release();
      THEN("the Fd is empty but the caller can still close the descriptor") {
        REQUIRE(released == watched);
        REQUIRE(!fd.IsOpen());
        REQUIRE(IsOpen(watched));
        REQUIRE(::close(watched) == 0);
        AND_WHEN("the empty Fd is released again") {
          const int released_again = fd.Release();
          THEN("it returns the invalid descriptor") {
            REQUIRE(released_again == Fd::kInvalid);
          }
        }
      }
    }
  }
}

SCENARIO("Closing an Fd is idempotent", "[fd]") {
  GIVEN("an Fd owning an open descriptor") {
    Fd fd = OpenNull();
    const int watched = fd.get();
    WHEN("it is closed") {
      fd.Close();
      THEN("the owner is empty and the descriptor is closed") {
        REQUIRE(!fd.IsOpen());
        REQUIRE(IsClosed(watched));
      }
      AND_WHEN("it is closed again") {
        fd.Close();
        THEN("it remains empty") { REQUIRE(!fd.IsOpen()); }
      }
    }
  }
}

SCENARIO("Destroying an empty Fd leaves errno unchanged", "[fd]") {
  GIVEN("errno is clear") {
    errno = 0;
    WHEN("an empty Fd leaves scope") {
      {
        const Fd empty;
      }
      THEN("errno is still clear") { REQUIRE(errno == 0); }
    }
  }
}

SCENARIO("An optional containing an empty Fd is still engaged", "[fd]") {
  GIVEN("an optional containing a default-constructed Fd") {
    std::optional<Fd> engaged{Fd{}};
    THEN("it has a value without owning a descriptor") {
      REQUIRE(engaged.has_value());
      REQUIRE(static_cast<bool>(engaged));
      REQUIRE(!engaged->IsOpen());
      REQUIRE(engaged->get() == Fd::kInvalid);
    }
    WHEN("it is compared with a disengaged optional") {
      const std::optional<Fd> none;
      THEN("only the disengaged optional equals nullopt") {
        REQUIRE(!none.has_value());
        REQUIRE(none == std::nullopt);
        REQUIRE(!(engaged == std::nullopt));
      }
    }
  }
}

SCENARIO("Optional Fd construction and reset manage ownership", "[fd]") {
  GIVEN("optionals constructed without a value") {
    const std::optional<Fd> none;
    const std::optional<Fd> from_nullopt{std::nullopt};
    THEN("both are disengaged") {
      REQUIRE(!none.has_value());
      REQUIRE(!from_nullopt.has_value());
    }
  }
  GIVEN("an optional constructed in place with an open descriptor") {
    std::optional<Fd> inplace{std::in_place, OpenRaw()};
    REQUIRE(inplace.has_value());
    const int watched = inplace->get();
    REQUIRE(IsOpen(watched));

    WHEN("it is reset") {
      inplace.reset();
      THEN("it disengages and closes the descriptor") {
        REQUIRE(!inplace.has_value());
        REQUIRE(IsClosed(watched));
      }
      AND_WHEN("it is reset again") {
        inplace.reset();
        THEN("it remains disengaged") { REQUIRE(!inplace.has_value()); }
      }
    }
  }
}

SCENARIO("An optional Fd closes its descriptor on destruction", "[fd]") {
  GIVEN("an optional owning an open descriptor") {
    int watched = Fd::kInvalid;
    WHEN("the optional leaves scope") {
      {
        const std::optional<Fd> held{OpenNull()};
        watched = held->get();
        REQUIRE(IsOpen(watched));
      }
      THEN("the descriptor is closed") { REQUIRE(IsClosed(watched)); }
    }
  }
}

SCENARIO("Moving an optional Fd transfers ownership and disengages the source",
         "[fd]") {
  GIVEN("an optional owning an open descriptor") {
    std::optional<Fd> source{OpenNull()};
    const int watched = source->get();

    WHEN("another optional is move-constructed from it") {
      std::optional<Fd> moved = std::move(source);
      THEN("the destination owns the descriptor and the source is disengaged") {
        REQUIRE(moved.has_value());
        REQUIRE(moved->get() == watched);
        REQUIRE(!source.has_value());
        REQUIRE(IsOpen(watched));
      }

      AND_WHEN(
          "it is move-assigned to an optional that owns another descriptor") {
        std::optional<Fd> target{OpenNull()};
        const int replaced = target->get();
        target = std::move(moved);
        THEN(
            "the old descriptor closes and the moved-from optional "
            "disengages") {
          REQUIRE(IsClosed(replaced));
          REQUIRE(target->get() == watched);
          REQUIRE(!moved.has_value());
        }
      }
    }
  }
}

SCENARIO("Assigning nullopt releases an optional Fd", "[fd]") {
  GIVEN("an optional owning an open descriptor") {
    std::optional<Fd> held{OpenNull()};
    const int watched = held->get();
    WHEN("nullopt is assigned") {
      held = std::nullopt;
      THEN("the optional disengages and closes the descriptor") {
        REQUIRE(!held.has_value());
        REQUIRE(IsClosed(watched));
      }
    }
  }
}

SCENARIO("Emplacing and swapping optional Fds preserves ownership", "[fd]") {
  GIVEN("a disengaged optional") {
    std::optional<Fd> held;
    WHEN("an open descriptor is emplaced") {
      Fd& placed = held.emplace(OpenRaw());
      REQUIRE(held.has_value());
      const int first = placed.get();

      AND_WHEN("another descriptor is emplaced") {
        held.emplace(OpenRaw());
        THEN("the previous descriptor is closed") { REQUIRE(IsClosed(first)); }

        AND_WHEN("the value is swapped into a disengaged optional") {
          std::optional<Fd> other;
          const int second = held->get();
          held.swap(other);
          THEN("ownership and engagement move to the other optional") {
            REQUIRE((other.has_value() && other->get() == second));
            REQUIRE(!held.has_value());
          }

          AND_WHEN("std::swap moves the value back") {
            std::swap(held, other);
            THEN("the original optional owns the descriptor again") {
              REQUIRE((held.has_value() && held->get() == second));
              REQUIRE(!other.has_value());
            }
          }
        }
      }
    }
  }
}

SCENARIO("Accessing an optional Fd value preserves engagement", "[fd]") {
  GIVEN("an optional owning an open descriptor") {
    std::optional<Fd> held{OpenNull()};
    WHEN("its value is accessed through mutable and const references") {
      const std::optional<Fd>& as_const = held;
      THEN("both references expose the stored descriptor") {
        REQUIRE(held.value().get() == held->get());
        REQUIRE(as_const.value().get() == held->get());
      }
    }
    WHEN("the contained Fd is moved out through value()") {
      Fd taken = std::move(held).value();
      THEN(
          "ownership transfers but the optional stays engaged with an empty "
          "Fd") {
        REQUIRE(taken.IsOpen());
        REQUIRE(held.has_value());
        REQUIRE(held->get() == Fd::kInvalid);
        REQUIRE(!held->IsOpen());
      }
    }
  }
}

SCENARIO("Optional Fd value_or selects the owned value or a fallback", "[fd]") {
  GIVEN("a disengaged optional") {
    std::optional<Fd> none;
    WHEN("value_or is given an open fallback") {
      Fd fallback = std::move(none).value_or(OpenNull());
      THEN("the fallback is returned") { REQUIRE(fallback.IsOpen()); }
    }
  }
  GIVEN("an optional owning an open descriptor") {
    std::optional<Fd> held{OpenNull()};
    const int watched = held->get();
    WHEN("value_or is given an empty fallback") {
      Fd kept = std::move(held).value_or(Fd{});
      THEN("the owned descriptor is returned") {
        REQUIRE(kept.get() == watched);
      }
    }
  }
}

SCENARIO("Optional Fd supports mapping, chaining and recovery", "[fd]") {
  GIVEN("an engaged optional and a disengaged optional") {
    std::optional<Fd> held{OpenNull()};
    const int watched = held->get();
    std::optional<Fd> none;

    WHEN("their values are transformed to descriptor numbers") {
      const auto to_number = [](const Fd& fd) { return fd.get(); };
      auto mapped = held.transform(to_number);
      static_assert(std::is_same_v<decltype(mapped), std::optional<int>>);
      THEN("only the engaged optional produces a number") {
        REQUIRE((mapped.has_value() && *mapped == watched));
        REQUIRE(!none.transform(to_number).has_value());
      }
    }
    WHEN("their values are chained to optional descriptor numbers") {
      const auto to_optional_number = [](const Fd& fd) {
        return std::optional<int>{fd.get()};
      };
      THEN("only the engaged optional produces a value") {
        REQUIRE(held.and_then(to_optional_number) == watched);
        REQUIRE(!none.and_then(to_optional_number).has_value());
      }
    }
    WHEN("the disengaged optional is given a recovery function") {
      auto recovered =
          std::move(none).or_else([] { return std::optional<Fd>{OpenNull()}; });
      THEN("the recovered optional is engaged") {
        REQUIRE(recovered.has_value());
      }
    }
  }
}
}

SCENARIO("The descriptor pool counts owned descriptors", "[fd]") {
  using cutils::os::Fd;
  using cutils::os::limits::fd::Pool;

  GIVEN("the current live descriptor count") {
    const auto start = Pool::Live();
    REQUIRE(!Fd{}.IsOpen());
    REQUIRE(Pool::Live() == start);

    WHEN("descriptors are opened, closed repeatedly and move-constructed") {
      {
        Fd a = OpenNull();
        REQUIRE(a.IsOpen());
        REQUIRE(Pool::Live() == start + 1);
        {
          Fd b = OpenNull();
          REQUIRE(Pool::Live() == start + 2);
          b.Close();
          REQUIRE(Pool::Live() == start + 1);
          b.Close();
          REQUIRE(Pool::Live() == start + 1);
        }
        REQUIRE(Pool::Live() == start + 1);

        Fd moved = std::move(a);
        REQUIRE(moved.IsOpen());
        REQUIRE(Pool::Live() == start + 1);
      }
      THEN("destruction returns the count to its starting value") {
        REQUIRE(Pool::Live() == start);
      }
    }
    WHEN("move assignment replaces an owned descriptor") {
      {
        Fd a = OpenNull();
        Fd b = OpenNull();
        REQUIRE(Pool::Live() == start + 2);
        b = std::move(a);
        REQUIRE(Pool::Live() == start + 1);
      }
      THEN("destruction returns the count to its starting value") {
        REQUIRE(Pool::Live() == start);
      }
    }
    WHEN("ownership is released to a raw descriptor") {
      {
        Fd a = OpenNull();
        REQUIRE(Pool::Live() == start + 1);
        const int raw = a.Release();
        REQUIRE(Pool::Live() == start);
        REQUIRE(::close(raw) == 0);
      }
      THEN("the raw descriptor is no longer counted") {
        REQUIRE(Pool::Live() == start);
      }
    }
    WHEN("an optional acquires and resets an owned descriptor") {
      {
        const std::optional<Fd> none;
        REQUIRE(Pool::Live() == start);
        std::optional<Fd> held{OpenNull()};
        REQUIRE(Pool::Live() == start + 1);
        held.reset();
        REQUIRE(Pool::Live() == start);
      }
      THEN("destruction leaves the count at its starting value") {
        REQUIRE(Pool::Live() == start);
      }
    }
  }
}
