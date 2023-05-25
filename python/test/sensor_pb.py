#! /usr/bin/env python
# -*- coding: utf-8 -*-
# ======================================================================
#
# sensor_pb.py -
#
# Created by liubang on 2023/05/26 00:44
# Last Modified: 2023/05/26 00:44
#
# ======================================================================
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
