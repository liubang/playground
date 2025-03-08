#! /usr/bin/env python
# -*- coding: utf-8 -*-

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


