#! /usr/bin/env python
# -*- coding: utf-8 -*-
# ======================================================================
#
# test_ut.py -
#
# Created by liubang on 2023/05/26 00:42
# Last Modified: 2023/05/26 00:42
#
# ======================================================================
import unittest


class TestSum(unittest.TestCase):

    def test_sum(self):
        self.assertEqual(sum([1, 2, 3]), 6, "Should be 6")

    def test_sum_tuple(self):
        self.assertEqual(sum((1, 5, 2)), 8, "Should be 6")


if __name__ == '__main__':
    unittest.main()
