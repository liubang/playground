// Copyright (c) 2025 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include <list>
#include <utility>

namespace pl {

template <typename K, typename V> class LruCache {
public:
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<key_type, mapped_type>;
    using list_type = std::list<value_type>;
    using size_type = typename list_type::size_type;
    using iterator = typename list_type::iterator;
    using const_iterator = typename list_type::const_iterator;

public:
};

} // namespace pl
