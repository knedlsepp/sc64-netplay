BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: chat.z64
.PHONY: all

OBJS = $(BUILD_DIR)/chat.o

chat.z64: N64_ROM_TITLE = "N64 Chat"

$(BUILD_DIR)/chat.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
