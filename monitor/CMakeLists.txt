# The CYD as nothing but a monitor.
#
# A second, separate firmware, and the reason it exists is memory rather than
# taste.  Under ArgonOS this board can give one application 123 KB of
# byte-addressable RAM in the shapes it can use - a 98 KB process heap, which
# is a single allocation and cannot exceed the largest free block, plus about
# 25 KB outside it - and a 320x240 frame is 150 KB.  Measured, not guessed.
#
# So a frame cannot be assembled whole there, and a frame that cannot be
# assembled whole reaches the glass as stripes, which is the thing this is all
# for.  With no process model there is one pool of some 300 KB and the frame
# fits with room to spare.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(argon_monitor)
