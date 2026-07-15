#!/bin/bash

# S频段天线管理面软件 - 快速启动脚本

echo "=========================================="
echo "  S频段天线管理面软件 - 快速启动"
echo "=========================================="
echo ""

# 检查是否已编译
if [ ! -f "./antenna_mgmt" ]; then
    echo "程序未编译，开始编译..."
    make clean
    make
    if [ $? -ne 0 ]; then
        echo "编译失败！"
        exit 1
    fi
    echo "编译成功！"
    echo ""
fi

# 创建日志目录
mkdir -p logs

# 检查配置文件
if [ ! -f "./config/antenna_mgmt.conf" ]; then
    echo "错误：配置文件不存在！"
    exit 1
fi

echo "启动程序..."
echo ""

# 运行程序
./antenna_mgmt ./config/antenna_mgmt.conf
