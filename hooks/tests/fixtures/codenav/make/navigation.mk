include config.mk
.PHONY: all
all: counts
align: reads
	@echo align
counts: align
	@echo count
define normalize
$(strip $(1))
endef
LABEL := $(call normalize, reads)
