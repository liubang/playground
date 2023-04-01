#======================================================================
#
# Makefile -
#
# Created by liubang on 2023/03/26 21:26
# Last Modified: 2023/03/26 21:26
#
#======================================================================

ROOTDIR := $(shell pwd)
OUTDIR  := $(ROOTDIR)/output
GO      := go
GOBUILD := $(GO) build
RM      := rm -rf

all:
	$(GOBUILD) -o $(OUTDIR)/bin/spider $(ROOTDIR)/spider/cmd/spider.go

clean:
	$(RM) $(OUTDIR)
