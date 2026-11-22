BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: lobby.z64
.PHONY: all

OBJS = $(BUILD_DIR)/lobby.o

lobby.z64: N64_ROM_TITLE = "N64 Lobby Demo"

$(BUILD_DIR)/lobby.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
