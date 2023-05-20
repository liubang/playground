#pragma once

#include <cstddef>

namespace playground::cpp::meta {
//-------------------------------------------------------------------------------------------------

// 定义iterator基本类型
template <typename Category,
          typename Tp,
          typename Distance = std::ptrdiff_t,
          typename Pointer = Tp*,
          typename Ref = Tp&>
struct iterator {
  using iterator_category = Category;
  using value_type = Tp;
  using difference_type = Distance;
  using pointer = Pointer;
  using reference = Ref;
};

// iterator tag
struct input_iterator_tag {};
struct forward_iterator_tag : input_iterator_tag {};
struct bidirectional_iterator_tag : forward_iterator_tag {};
struct random_access_iterator_tag : bidirectional_iterator_tag {};

// iterator traits
template <typename Iterator>
struct iterator_traits {
  using iterator_category = typename Iterator::iterator_category;
  using value_type = typename Iterator::value_type;
  using difference_type = typename Iterator::difference_type;
  using pointer = typename Iterator::pointer;
  using reference = typename Iterator::reference;
};

template <typename Iterator>
void advanceImpl(Iterator& iter,
                 typename iterator_traits<Iterator>::difference_type n,
                 input_iterator_tag) {
  for (; n > 0; --n)
    ++iter;
}

template <typename Iterator>
void advanceImpl(Iterator& iter,
                 typename iterator_traits<Iterator>::difference_type n,
                 bidirectional_iterator_tag) {
  if (n >= 0)
    for (; n > 0; --n)
      ++iter;
  else
    for (; n < 0; ++n)
      --iter;
}

template <typename Iterator>
void advanceImpl(Iterator& iter,
                 typename iterator_traits<Iterator>::difference_type n,
                 random_access_iterator_tag) {
  iter += n;
}

template <typename Iterator>
void advance(Iterator& iter,
             typename iterator_traits<Iterator>::difference_type n) {
  advanceImpl(iter, n, iterator_traits<Iterator>::iterator_category);
}

};  // namespace highkyck::meta
