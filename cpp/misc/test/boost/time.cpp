// Copyright (c) 2023 The Authors. All rights reserved.
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

#include <boost/date_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <iostream>

int main(int argc, char* argv[]) {

    std::cout.imbue(std::locale(std::cout.getloc(),
                                new boost::local_time::local_time_facet("%Y-%m-%dT%H:%M:%S%F%Q")));
    std::cout << boost::local_time::local_microsec_clock::local_time(
                     boost::local_time::time_zone_ptr())
              << std::endl;

    std::cout << "=================================\n";

    boost::gregorian::date date = boost::gregorian::from_string("2023-12-23 10:43:53");

    std::cout << boost::gregorian::to_simple_string(date) << "\n";
    std::cout << boost::gregorian::to_iso_string(date) << "\n";
    std::cout << boost::gregorian::to_iso_extended_string(date) << "\n";

    return 0;
}
