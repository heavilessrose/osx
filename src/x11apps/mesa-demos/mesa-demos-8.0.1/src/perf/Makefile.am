# Copyright © 2010 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
# Authors:
#    Eric Anholt <eric@anholt.net>

if HAVE_GLUT
noinst_LTLIBRARIES = libperf.la

libperf_la_SOURCES = \
	common.c \
	common.h \
	glmain.c \
	glmain.h

AM_CFLAGS = \
	$(DEMO_CFLAGS) \
	$(GLUT_CFLAGS) \
	-I$(top_srcdir)/src/util

AM_LDFLAGS = \
	$(DEMO_LIBS) \
	$(GLUT_LIBS)

bin_PROGRAMS = \
	copytex \
	drawoverhead \
	fbobind \
	fill \
	genmipmap \
	readpixels \
	swapbuffers \
	teximage \
	vbo \
	vertexrate
endif

copytex_LDADD = libperf.la
drawoverhead_LDADD = libperf.la
fbobind_LDADD = libperf.la
fill_LDADD = libperf.la
genmipmap_LDADD = libperf.la
readpixels_LDADD = libperf.la
swapbuffers_LDADD = libperf.la
teximage_LDADD = libperf.la
vbo_LDADD = libperf.la
vertexrate_LDADD = libperf.la
