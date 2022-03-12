#include <array>
#include <iostream>
#include <list>
#include <string>
#include <tuple>
#include <vector>

#define Forward(...) std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

template <typename UnknownType, typename ReferenceType>
concept SubtypeOf = std::same_as<std::decay_t<UnknownType>, ReferenceType> ||
    std::derived_from<std::decay_t<UnknownType>, ReferenceType>;

template <typename UnknownType, typename... ReferenceTypes>
concept AnyOf = (SubtypeOf<UnknownType, ReferenceTypes> || ...);

template <typename UnknownType, typename... ReferenceTypes>
concept AnyBut = !AnyOf<UnknownType, ReferenceTypes...>;

template <typename UnknownType, typename ReferenceType>
concept ExplicitlyConvertibleTo = requires(UnknownType x) {
  static_cast<ReferenceType>(Forward(x));
};

template <typename UnknownType, typename ReferenceType>
concept ConstructibleFrom =
    ExplicitlyConvertibleTo<ReferenceType, std::decay_t<UnknownType>>;

template <typename UnknownType>
concept BuiltinArray = std::is_array_v<std::remove_cvref_t<UnknownType>>;

template <typename UnknownType>
concept Advanceable = requires(UnknownType x) {
  ++x;
};

template <typename UnknownType>
concept Iterable = BuiltinArray<UnknownType> || requires(UnknownType x) {
  { x.begin() } -> Advanceable;
  { *x.begin() } -> AnyBut<void>;
  { x.begin() != x.end() } -> ExplicitlyConvertibleTo<bool>;
};

template <typename UnknownType>
struct _ExtractInnermostElementType {
  using Type = UnknownType;
};

template <Iterable UnknownType>
requires(BuiltinArray<UnknownType> ==
         false) struct _ExtractInnermostElementType<UnknownType> {
  using ElementType =
      std::remove_cvref_t<decltype(*std::declval<UnknownType>().begin())>;
  using Type = _ExtractInnermostElementType<ElementType>::Type;
};

template <typename UnknownType, auto Length>
struct _ExtractInnermostElementType<UnknownType[Length]> {
  using ElementType = std::remove_cvref_t<UnknownType>;
  using Type = _ExtractInnermostElementType<ElementType>::Type;
};

template <typename UnknownType>
using ExtractInnermostElementType =
    _ExtractInnermostElementType<std::remove_cvref_t<UnknownType>>::Type;

auto& operator<<(SubtypeOf<std::ostream> auto& Printer, Iterable auto&& Container) requires(
    requires {
      Printer
          << std::declval<ExtractInnermostElementType<decltype(Container)>>();
    } &&
    requires {
      { Container } -> ConstructibleFrom<const char*>;
    } == false) {
  auto [Startpoint, Endpoint] = [&] {
    if constexpr (requires {
                    { Container } -> BuiltinArray;
                  })
      return std::tuple{Container,
                        Container + sizeof(Container) / sizeof(Container[0])};
    else
      return std::tuple{Container.begin(), Container.end()};
  }();
  Printer << "[";
  for (auto Cursor = Startpoint; Cursor != Endpoint; ++Cursor)
    if (Cursor != Startpoint)
      Printer << ", " << *Cursor;
    else
      Printer << *Cursor;
  Printer << "]";
  return Printer;
}

auto main() -> int {
  using namespace std::literals;

  int x[][3] = {{1, 2, 3}, {42, 21}};
  auto y = std::vector<int>{};
  auto z = std::array{3.14, 2.71, 1.414};
  auto w = std::vector{std::list{"hello", "world"}, std::list{"aaa", "bbb"}};
  auto v = std::array{"I"s, "want"s, "constexpr"s, "function"s, "params"s};

  std::cout << x << std::endl;  // prints [[1, 2, 3], [42, 21, 0]]
  std::cout << y << std::endl;  // prints []
  std::cout << z << std::endl;  // prints [3.14, 2.71, 1.414]
  std::cout << w << std::endl;  // prints [[hello, world], [aaa, bbb]]
  std::cout << v << std::endl;  // prints [I, want, constexpr, function, params]
}
