.. SPDX-License-Identifier: GPL-2.0

======
zblock
======

zblock is a special purpose allocator for storing compressed pages.
It stores integer number of compressed objects per its block. These
blocks consist of several physical pages (2**n, i. e. 1/2/4/8).

With zblock, it is possible to densely arrange objects of various sizes
resulting in low internal fragmentation. Also this allocator tries to
fill incomplete blocks instead of adding new ones,  in many cases
providing a compression ratio substantially higher than z3fold and zbud
(though lower than zmalloc's).

zblock does not require MMU to operate and also is superior to zsmalloc
with regard to average performance and worst execution times, thus
allowing for better response time and real-time characteristics of the
whole system.

E. g. on a series of stress-ng tests run on a Raspberry Pi 5, we get
5-10% higher value for bogo ops/s in zblock/zsmalloc comparison.

