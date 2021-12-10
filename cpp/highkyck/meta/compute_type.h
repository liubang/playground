namespace highkyck {
namespace detail {

// 使用模板进行类型计算
template <typename T>
struct PointerType {
  using type = T*;
};

// typename 表示输入的参数是类型
template <typename T>
using PointerType_t = typename PointerType<T>::type;

}  // namespace detail
}  // namespace highkyck
