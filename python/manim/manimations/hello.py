#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2025 The Authors. All rights reserved.
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
# Created: 2025/03/08 22:31

from manim import *
import math

class HelloWorld(Scene):
    def construct(self):
        # 添加文字
        text = Text("Hello World", font_size=72)
        text.to_edge(UP)
        self.play(Write(text), run_time=3)

        # 圆形
        circle = Circle()
        circle.set_stroke(width=4)
        # 用 ValueTracker 和 updater 来模拟运动
        t = ValueTracker(0)

        def update_circle(mob):
            mob.move_to(math.cos(t.get_value()) * RIGHT)

        circle.add_updater(update_circle)

        # 正方形跟随圆形
        square = Square()

        def update_square(mob):
            mob.move_to(circle.get_center() + DOWN)

        square.add_updater(update_square)

        self.add(circle, square)

        # 动态运行 10 秒钟
        self.play(t.animate.increment_value(10), run_time=10, rate_func=linear)

        # 移除 updater 以便后续操作不会被干扰
        circle.clear_updaters()
        square.clear_updaters()

        # 添加 3D 坐标轴（仅演示用，Scene 中其实不会显示出 3D 效果）
        axes = ThreeDAxes()
        self.add(axes)
        self.wait()
