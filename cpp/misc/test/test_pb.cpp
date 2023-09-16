//=====================================================================
//
// test_pb.cpp -
//
// Created by liubang on 2023/05/25 23:51
// Last Modified: 2023/05/25 23:51
//
//=====================================================================
#include "proto/test/sensor.pb.h"
#include <fstream>

int main() {
    proto::test::Sensor sensor;
    sensor.set_name("Laboratory");
    sensor.set_temperature(23.4);
    sensor.set_humidity(68);
    sensor.set_door(proto::test::Sensor_SwitchLevel_OPEN);
    std::ofstream ofs("sensor.data", std::ios_base::out | std::ios_base::binary);
    sensor.SerializeToOstream(&ofs);
}
