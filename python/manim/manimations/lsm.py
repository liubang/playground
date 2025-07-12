# -*- coding: utf-8 -*-

from manim import *

class LevelDBWriteAndCompaction(Scene):
    def construct(self):
        title = Text("LevelDB 写入流程与 Compaction", font_size=48)
        self.play(Write(title))
        self.wait(1)
        self.play(title.animate.to_edge(UP))

        # Step 1: WAL
        wal = Rectangle(width=2.5, height=1.0, color=BLUE).shift(LEFT * 4)
        wal_text = Text("WAL", font_size=24).move_to(wal)
        self.play(Create(wal), Write(wal_text))

        # Step 2: MemTable
        mem = Rectangle(width=2.5, height=1.0, color=GREEN).next_to(wal, RIGHT, buff=1.5)
        mem_text = Text("MemTable", font_size=24).move_to(mem)
        self.play(Create(mem), Write(mem_text))

        # Write Flow Arrow
        write_arrow1 = Arrow(start=wal.get_right(), end=mem.get_left(), buff=0.1)
        self.play(GrowArrow(write_arrow1))
        write_label = Text("写入", font_size=20).next_to(write_arrow1, UP)
        self.play(Write(write_label))

        self.wait(1)

        # Step 3: MemTable Full -> Immutable
        imm = Rectangle(width=2.5, height=1.0, color=YELLOW).next_to(mem, DOWN, buff=1.2)
        imm_text = Text("Immutable MemTable", font_size=20).move_to(imm)
        self.play(FadeIn(imm), Write(imm_text))

        flush_arrow = Arrow(start=mem.get_bottom(), end=imm.get_top(), buff=0.1)
        flush_label = Text("Flush", font_size=20).next_to(flush_arrow, LEFT)
        self.play(GrowArrow(flush_arrow), Write(flush_label))
        self.wait(1)

        # Step 4: L0 SSTable
        l0 = Rectangle(width=2.5, height=1.0, color=ORANGE).next_to(imm, DOWN, buff=1.2)
        l0_text = Text("SSTable (L0)", font_size=20).move_to(l0)
        self.play(Create(l0), Write(l0_text))

        sst_arrow = Arrow(start=imm.get_bottom(), end=l0.get_top(), buff=0.1)
        self.play(GrowArrow(sst_arrow))

        self.wait(2)

        # ===== Compaction Section =====
        compaction_title = Text("Compaction 过程", font_size=36)
        self.play(FadeOut(wal), FadeOut(wal_text), FadeOut(write_arrow1), FadeOut(write_label),
                  FadeOut(mem), FadeOut(mem_text), FadeOut(imm), FadeOut(imm_text),
                  FadeOut(flush_arrow), FadeOut(flush_label), FadeOut(sst_arrow),
                  title.animate.shift(UP*0.5),
                  FadeIn(compaction_title.shift(DOWN*2)))
        self.wait(1)
        self.play(FadeOut(compaction_title))

        # L0 -> L1 compaction
        l1 = Rectangle(width=3.0, height=1.0, color=PURPLE).next_to(l0, DOWN, buff=1.2)
        l1_text = Text("SSTable (L1)", font_size=20).move_to(l1)

        comp_arrow = Arrow(start=l0.get_bottom(), end=l1.get_top(), buff=0.1)
        comp_label = Text("Compaction", font_size=20).next_to(comp_arrow, RIGHT)

        self.play(Create(comp_arrow), Write(comp_label))
        self.play(Create(l1), Write(l1_text))

        # 添加说明
        # desc = Tex(r"合并范围重叠的文件，清理过期数据", font_size=28)
        desc = Text("合并范围重叠的文件，清理过期数据", font_size=28)
        desc.next_to(l1, DOWN, buff=1.0)
        self.play(Write(desc))
        self.wait(3)

        # 结束
        self.play(*[FadeOut(mob) for mob in self.mobjects])
