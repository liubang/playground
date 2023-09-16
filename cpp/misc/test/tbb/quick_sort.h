//=====================================================================
//
// quick_sort.h -
//
// Created by liubang on 2023/06/18 15:30
// Last Modified: 2023/06/18 15:30
//
//=====================================================================
#pragma once

#include <tbb/parallel_invoke.h>

#include <algorithm>
#include <functional>

namespace pl {

template <typename T> void quick_sort(T *data, size_t size) {
    if (size < 1)
        return;
    size_t mid = std::hash<size_t>{}(size);
    mid ^= std::hash<void *>{}(static_cast<void *>(data));
    mid %= size;
    std::swap(data[0], data[mid]);
    T pivot = data[0];
    size_t left = 0, right = size - 1;
    while (left < right) {
        while (left < right && !(data[right] < pivot))
            right--;
        if (left < right)
            data[left++] = data[right];
        while (left < right && data[left] < pivot)
            left++;
        if (left < right)
            data[right--] = data[left];
    }
    data[left] = pivot;
    quick_sort(data, left);
    quick_sort(data + left + 1, size - left - 1);
}

template <typename T> void quick_sort2(T *data, size_t size) {
    if (size < 1)
        return;
    size_t mid = std::hash<size_t>{}(size);
    mid ^= std::hash<void *>{}(static_cast<void *>(data));
    mid %= size;
    std::swap(data[0], data[mid]);
    T pivot = data[0];
    size_t left = 0, right = size - 1;
    while (left < right) {
        while (left < right && !(data[right] < pivot))
            right--;
        if (left < right)
            data[left++] = data[right];
        while (left < right && data[left] < pivot)
            left++;
        if (left < right)
            data[right--] = data[left];
    }
    data[left] = pivot;
    tbb::parallel_invoke(
        [&] {
            quick_sort(data, left);
        },
        [&] {
            quick_sort(data + left + 1, size - left - 1);
        });
}

template <typename T> void quick_sort3(T *data, size_t size) {
    if (size < 1)
        return;
    if (size < (1 << 16)) {
        std::sort(data, data + size, std::less<T>{});
        return;
    }
    size_t mid = std::hash<size_t>{}(size);
    mid ^= std::hash<void *>{}(static_cast<void *>(data));
    mid %= size;
    std::swap(data[0], data[mid]);
    T pivot = data[0];
    size_t left = 0, right = size - 1;
    while (left < right) {
        while (left < right && !(data[right] < pivot))
            right--;
        if (left < right)
            data[left++] = data[right];
        while (left < right && data[left] < pivot)
            left++;
        if (left < right)
            data[right--] = data[left];
    }
    data[left] = pivot;
    tbb::parallel_invoke(
        [&] {
            quick_sort(data, left);
        },
        [&] {
            quick_sort(data + left + 1, size - left - 1);
        });
}

} // namespace pl
