# -*- coding: utf-8 -*-

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
