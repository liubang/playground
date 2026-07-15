# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/07/15 13:28

alias(
    name = "spark_extension_jar",
    actual = "//java/pl/bigdata/spark-extensions:spark_extension_jar",
    visibility = ["//visibility:public"],
)

alias(
    name = "trino_plugin_zip",
    actual = "//java/pl/bigdata/trino-extensions:trino_plugin_zip",
    visibility = ["//visibility:public"],
)

test_suite(
    name = "tests",
    tests = [
        "//java/pl/bigdata/spark-extensions:ReverseUdfTest",
        "//java/pl/bigdata/trino-extensions:E2eFunctionsTest",
    ],
)
