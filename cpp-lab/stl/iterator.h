//=====================================================================
//
// iterator.h -
//
// Created by liubang on 2020/02/07
// Last Modified: 2020/02/07 20:59
//
//=====================================================================
#pragma once

#include <cstddef>

namespace liubang {

struct input_iterator_tag {};                                              // 返回输入迭代器
struct output_iterator_tag {};                                             // 返回输出迭代器
struct forward_iterator_tag : public input_iterator_tag {};                // 返回向前迭代器
struct bidirectional_iterator_tag : public forward_iterator_tag {};        // 返回双向迭代器
struct random_access_iterator_tag : public bidirectional_iterator_tag {};  // 返回随机迭代器

// 输入迭代器
// 通过对输入迭代器解除引用，它将引用对象，而对象可能位于集合中。通常用于传递地址。
template <class T, class Distance>
struct input_iterator {
  typedef input_iterator_tag iterator_category;  // 返回类型
  typedef T value_type;                          // 所指对象类型
  typedef Distance difference_type;              // 迭代器间距离类型
  typedef T* pointer;                            // 操作结果类型
  typedef T& reference;                          // 解引用操作结果类型
};

// 输出迭代器
// 改类迭代器和输入迭代器及其相似，也只能单步向前迭代元素，
// 不同的是，它只有读的权限，通常用于返回地址。
template <class T, class Distance>
struct output_iterator {
  typedef output_iterator_tag iterator_category;
  typedef void value_type;
  typedef void difference_type;
  typedef void pointer;
  typedef void reference;
};

// 向前迭代器
// 可以在一个正确的区间中进行读写操作，它拥有输入迭代器的所有特性，和输出迭代器的部分特性
// 以及单步向前迭代元素的能力。
template <class T, class Distance>
struct forward_iterator {
  typedef forward_iterator_tag iterator_category;
  typedef T value_type;
  typedef Distance difference_type;
  typedef T* pointer;
  typedef T& reference;
};

// 双向迭代器
// 在向前迭代器的基础上提供了单步向后迭代的能力，是向前迭代器的高级版。
template <class T, class Distance>
struct bidirectional_iterator {
  typedef bidirectional_iterator_tag iterator_category;
  typedef T value_type;
  typedef Distance difference_type;
  typedef T* pointer;
  typedef T& reference;
};

// 随机迭代器
template <class T, class Distance>
struct random_access_iterator {
  typedef random_access_iterator_tag iterator_category;
  typedef T value_type;
  typedef Distance difference_type;
  typedef T* pointer;
  typedef T& reference;
};

template <class Category, class T, class Distance = ptrdiff_t, class Pointer = T*, class Reference = T&>
struct iterator {
  typedef Category iterator_category;
  typedef T value_type;
  typedef Distance difference_type;
  typedef Pointer pointer;
  typedef Reference reference;
};

template <class Iterator>
struct iterator_traits {
  typedef typename Iterator::iterator_category iterator_category;
  typedef typename Iterator::value_type value_type;
  typedef typename Iterator::difference_type difference_type;
  typedef typename Iterator::pointer pointer;
  typedef typename Iterator::reference reference;
};

template <class T>
struct iterator_traits<T*> {
  typedef random_access_iterator_tag iterator_category;
  typedef T value_type;
  typedef ptrdiff_t difference_type;
  typedef T* pointer;
  typedef T& reference;
};

template <class T>
struct iterator_traits<const T*> {
  typedef random_access_iterator_tag iterator_category;
  typedef T value_type;
  typedef ptrdiff_t difference_type;
  typedef const T* pointer;
  typedef const T& reference;
};

template <class Iterator>
inline typename iterator_traits<Iterator>::iterator_category iterator_category(const Iterator& it) {
  typedef typename iterator_traits<Iterator>::iterator_category category;
  return category();
}

template <class Iterator>
inline typename iterator_traits<Iterator>::value_type* value_type(const Iterator& it) {
  return static_cast<typename iterator_traits<Iterator>::value_type*>(0);
}

template <class Iterator>
inline typename iterator_traits<Iterator>::difference_type* difference_type(const Iterator& it) {
  return static_cast<typename iterator_traits<Iterator>::difference_type*>(0);
}

}  // namespace liubang
