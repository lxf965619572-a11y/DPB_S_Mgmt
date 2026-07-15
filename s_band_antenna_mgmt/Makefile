# Makefile for S-Band Antenna Management Software

# 编译器和编译选项
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I./include
LDFLAGS = -lpthread -lssl -lcrypto

# 目录定义
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
BIN_DIR = .

# 目标程序
TARGET = $(BIN_DIR)/antenna_mgmt

# 源文件和目标文件
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# 默认目标
all: $(TARGET)

# 链接目标程序
$(TARGET): $(OBJS)
	@echo "Linking $@..."
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@"

# 编译源文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# 清理编译产物
clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR)/*.o $(TARGET)
	@echo "Clean complete"

# 安装
install: $(TARGET)
	@echo "Installing to /usr/local/bin..."
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod +x /usr/local/bin/antenna_mgmt
	@echo "Install complete"

# 运行
run: $(TARGET)
	./$(TARGET)

# 调试运行
debug: $(TARGET)
	gdb ./$(TARGET)

# 显示帮助
help:
	@echo "S-Band Antenna Management Software Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the program (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Install to /usr/local/bin"
	@echo "  run      - Build and run the program"
	@echo "  debug    - Run with gdb debugger"
	@echo "  help     - Show this help message"

.PHONY: all clean install run debug help
