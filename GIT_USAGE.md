# Git 使用指南

## 当前仓库状态

✅ Git 仓库已初始化  
✅ 已创建 3 个提交记录  
✅ 当前分支：master  

## 提交历史

```
* 2d5dd0a - 文档：添加项目README和更新gitignore
* 53c8946 - 更新至20260521版本
* 9435988 - 初始版本：S波段天线管理系统
```

## 常用 Git 命令

### 查看状态
```bash
git status              # 查看当前状态
git log --oneline       # 查看提交历史（简洁版）
git log --graph --all   # 查看提交历史（图形化）
```

### 添加和提交更改
```bash
git add <文件名>        # 添加特定文件
git add .               # 添加所有更改
git commit -m "提交信息"  # 提交更改
```

### 查看差异
```bash
git diff                # 查看未暂存的更改
git diff --staged       # 查看已暂存的更改
git diff HEAD~1         # 与上一次提交比较
```

### 分支管理
```bash
git branch              # 查看所有分支
git branch <分支名>     # 创建新分支
git checkout <分支名>   # 切换分支
git checkout -b <分支名> # 创建并切换到新分支
git merge <分支名>      # 合并分支
```

### 撤销操作
```bash
git restore <文件名>    # 撤销工作区的修改
git restore --staged <文件名>  # 取消暂存
git reset HEAD~1        # 撤销最后一次提交（保留更改）
git reset --hard HEAD~1 # 撤销最后一次提交（丢弃更改）⚠️
```

### 查看文件历史
```bash
git log --follow <文件名>  # 查看文件的修改历史
git blame <文件名>         # 查看文件每一行的修改者
git show <提交ID>          # 查看某次提交的详细信息
```

## 推荐工作流程

### 日常开发
1. 修改代码
2. `git status` 查看修改了哪些文件
3. `git diff` 查看具体修改内容
4. `git add <文件>` 暂存要提交的文件
5. `git commit -m "描述性提交信息"` 提交更改

### 开发新功能
1. `git checkout -b feature/新功能名` 创建特性分支
2. 在分支上进行开发和提交
3. 完成后切换回主分支：`git checkout master`
4. 合并特性分支：`git merge feature/新功能名`
5. 删除特性分支：`git branch -d feature/新功能名`

### 修复 Bug
1. `git checkout -b fix/bug描述` 创建修复分支
2. 修复 bug 并提交
3. 切换回主分支并合并

## 提交信息规范

建议使用以下格式：

- `新增: <描述>` - 添加新功能
- `修复: <描述>` - 修复 bug
- `更新: <描述>` - 更新现有功能
- `重构: <描述>` - 代码重构
- `文档: <描述>` - 文档更新
- `测试: <描述>` - 添加或修改测试
- `构建: <描述>` - 构建系统或依赖项更改

示例：
```bash
git commit -m "修复: 修复FPGA固件上注时的内存泄漏问题"
git commit -m "新增: 添加网络配置自动检测功能"
```

## 远程仓库（可选）

如果需要将代码推送到远程仓库（如 GitHub、GitLab 等）：

```bash
# 添加远程仓库
git remote add origin <远程仓库地址>

# 查看远程仓库
git remote -v

# 推送到远程仓库
git push -u origin master

# 从远程仓库拉取
git pull origin master
```

## 忽略文件配置

当前 `.gitignore` 已配置忽略：
- 编译产物（*.o, *.so, *.a, build/）
- 日志文件（*.log, logs/）
- 压缩包（*.zip, *.tar, *.gz）
- 临时文件
- 版本备份目录

## 注意事项

⚠️ **重要提示：**
1. 在使用 `git reset --hard` 等危险命令前，请确保已备份重要更改
2. 提交前请仔细检查 `git status` 和 `git diff` 的输出
3. 不要提交敏感信息（密码、密钥等）
4. 定期提交，保持提交粒度适中
5. 写清晰的提交信息，方便日后查看

## 帮助资源

```bash
git help                # 查看 git 帮助
git help <命令>         # 查看特定命令的帮助
```

---

创建日期：2026-07-15  
仓库位置：D:\lixf\S天线
