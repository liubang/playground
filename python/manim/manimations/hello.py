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

from manimlib import *
from manimlib.mobject.svg.old_tex_mobject import *


class HelloWorld(InteractiveScene):
    def construct(self):
        # Add some simple geometry
        text = Text("Hello World", font_size=72)
        text.to_edge(UP)
        self.play(Write(text, run_time=3))

        circle = Circle()
        circle.set_flat_stroke(True)
        circle.always.move_to(math.cos(self.time) * RIGHT)

        square = Square()
        square.always.move_to(circle, DOWN)

        self.add(circle)
        self.add(square)
        self.wait(10)

        self.add(ThreeDAxes())


