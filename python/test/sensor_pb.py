# Copyright (c) 2024 The Authors. All rights reserved.
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

#! /usr/bin/env python
# -*- coding: utf-8 -*-
from proto.test.sensor_pb2 import Sensor
import argparse

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='这是一个简单的程序示例')
    parser.add_argument("-f", "--file", type=str, help='文件路径')
    args = parser.parse_args()

    with open(args.file, 'rb') as file:
        content = file.read()
        print("Retrieve Sensor object from sensor.data")
        sensor = Sensor()
        sensor.ParseFromString(content)
        print(f"Sensor name: {sensor.name}")
        print(f"Sensor temperature: {sensor.temperature}")
        print(f"Sensor humidity: {sensor.humidity}")

    print("Sensor door: {}".format("Open" if sensor.door else "Closed"))
