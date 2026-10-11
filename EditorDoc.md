# AI Text Editor

我们将开发一款生产级、简单直接的 GUI 文本（代码）编辑器，支持 Windows、macOS 和 Linux（Ubuntu / Fedora / Arch)。这将是一款专业级的软件，必须具备出色的稳定性、完善的测试覆盖，并且能够真正部署到生产环境。不使用临时性的变通方案，不走捷径，不接受投机取巧的实现。整个引擎必须拥有坚实可靠的工程基础。

## Dev Workflow

- 仅在绝对必要时才提出问题，尽可能自主完成开发工作。
- 重要！严禁查看工作目录以外的任何内容，也不得参考我电脑上的其他代码。
- 不要参考任何记忆中的任何项目
- 首先询问我应该给编辑器起什么名字，并提供几个候选名称
- 测试至关重要。 必须编写大量单元测试，可以使用任何测试框架，简单易用即可，并且必须运行自动化测试。
- 所有工作都必须遵循规范的工程实践。这是一个生产级项目，而不是临时拼凑的实验性项目。
- 创建 AGENTS.md 以及相关的 Agent Skills，用于辅助开发，并包含具体、明确的开发规范和指导原则。
- 像人类一样提交代码，而不是一口气提交
- 使用 conventional git commit，不需要 commit msg body，commit msg 长度小于 50 个字符
- 代码需要提交并推送到 [github repo](https://github.com/Ariaszzzhc/astra-editor)

## 技术选型

- 编程语言：C++23、js；构建使用 [CMake](https://cmake.org/) + Ninja，依赖管理使用固定 commit 的 vcpkg submodule、manifest 和必要的 overlay ports。
- 代码高亮：[treesitter](https://tree-sitter.github.io/tree-sitter/)
- 窗口以及图形绘制：sdl3、skia、Graphite、Dawn
- Scripting: [quickjs-ng](https://github.com/quickjs-ng/quickjs)
- 事件循环：[libuv](https://libuv.org/)

## 核心数据结构

[Rope & SumTree](https://zed.dev/blog/zed-decoded-rope-sumtree)
[Rope Optimizations, Part 1](https://zed.dev/blog/zed-decoded-rope-optimizations-part-1)

## 设计哲学

"Everything's a SumTree"

## 基础架构

- client/server 同构，天然支持 remote。可以通过 server 模式监听，client 模式连接。也可以直接启动进程内调度
- C++ 为核心，js 高层 scripting，同时需要暴露 js api 可供用户定制化（如同 nvim 可以使用 lua 定制化）
- 编辑器要能够被 AI Agent 操控，以便你模拟进行编辑测试行为是否正确
- 代码风格参考 [Jason Turner](https://www.youtube.com/@cppweekly)
- 界面风格参考 vscode，开箱即用的情况下需要有 vscode 风味的体验

## 功能要求

- 完成编辑体验
- 文件树
- 工作区
- 全局代码搜索、替换
- LSP 支持
- 多光标
- textobject 操作
- git 集成
- outline
- vscode keymap
- editorconfig 集成
- request/response + notification 通信协议
- 文件 icon、theme 配色
- 上述的功能需要 out of box

## out of scope

- 如同 vscode live share 这样的协作
- 如同 vscode agent mode 这样的内嵌 agent
- 内嵌 terminal
- run、debug
