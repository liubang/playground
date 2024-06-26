// Copyright (c) 2024 The Authors. All rights reserved.
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

#include <boost/parameter.hpp>
#include <string>
#include <type_traits>

BOOST_PARAMETER_KEYWORD(tag, num_cpus);
BOOST_PARAMETER_KEYWORD(tag, num_gpus);
BOOST_PARAMETER_KEYWORD(tag, object_store_memory);
BOOST_PARAMETER_KEYWORD(tag, ignore_reinit_error);
BOOST_PARAMETER_KEYWORD(tag, address);

struct myconf {
    int num_cpus_ = 1;
    int num_gpus_ = 1;
    int object_store_memory_ = 16000;
    bool ignore_reinit_error_ = true;
    std::string address_ = "auto";
};

inline static void init_arg(
    myconf& conf, const boost::parameter::aux::tagged_argument<tag::num_cpus, const int>& arg) {
    conf.num_cpus_ = arg[num_cpus];
}

inline static void init_arg(
    myconf& conf, const boost::parameter::aux::tagged_argument<tag::num_gpus, const int>& arg) {
    conf.num_gpus_ = arg[num_gpus];
}

inline static void init_arg(
    myconf& conf,
    const boost::parameter::aux::tagged_argument<tag::object_store_memory, const int>& arg) {
    conf.object_store_memory_ = arg[object_store_memory];
}

inline static void init_arg(
    myconf& conf,
    const boost::parameter::aux::tagged_argument<tag::ignore_reinit_error, const bool>& arg) {
    conf.ignore_reinit_error_ = arg[ignore_reinit_error];
}

inline static void init_arg(
    myconf& conf,
    const boost::parameter::aux::tagged_argument<tag::address, const std::string>& arg) {
    conf.address_ = arg[address];
}

template <typename... Args> inline static void init(Args... args) {
    myconf conf{};
    (void)std::initializer_list<int>{(init_arg(conf, args), 0)...};
}

int main(int argc, char* argv[]) {
    const std::string addr = "127.0.0.1";
    init(num_cpus = 8, address = addr, ignore_reinit_error = true);
    init(num_gpus = 1, object_store_memory = 400, num_cpus = 8);
    return 0;
}
