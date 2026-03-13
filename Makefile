CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -D_GNU_SOURCE -O2 $(shell pkg-config --cflags srt)
LDFLAGS  = $(shell pkg-config --libs srt) -lpthread -lm

SRC_DIR  = src
BUILD_DIR= build
TARGET   = error-injector

SRCS     = $(wildcard $(SRC_DIR)/*.c)
OBJS     = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS     = $(OBJS:.o=.d)

.PHONY: all clean debug install

all: $(TARGET)

debug: CFLAGS := -Wall -Wextra -std=c11 -D_GNU_SOURCE -g -O0 -DDEBUG $(shell pkg-config --cflags srt)
debug: clean $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
