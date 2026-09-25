# 测试目录与注释整理验证（2026-09-25）

## 本次变更

按确认范围迁移 137 个文件：5 个维护脚本移至 tests/scripts，配置探针移至 tests/check_stdexec.cpp，131 个历史程序、脚本及夹具输入移至 tests/archive。完整旧路径、新路径和迁移前 SHA256 见 [归档索引](archive/README.md) 与 [迁移清单](archive/migration-manifest.json)。

当前测试、历史程序和测试辅助脚本均增加具体测试项目注释，说明场景、判据、入口及适用边界。JSON 夹具不能直接加注释，其用途在同目录 README 中说明。迁移修复源码/脚本输入、仓库根及相对 include；编译产物、生成依赖和日志仍留在 build。

C++ 注释使用 UTF-8 BOM，未因此增加公共编译选项。没有修改公共 API、生产状态机、依赖版本或测试断言。正式示例仍在 examples，通用构建工具仍在 scripts。

## 已执行的验证

从仓库根执行，日志位于 build/test-layout-20260925。

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| scripts/msvc-cmake.cmd test all | Debug 56/56；Release 56/56，均退出 0 | ctest-all.log；CTest 耗时 58.60 / 36.58 秒 |
| 新 Release 压力入口，pause_sender_test，Iterations=2 | 43 类 × 2 次，共 86 个完整场景，通过 | release-entry.log、release/*.result.json |
| 新 ASan 压力入口，相同参数 | 86 个完整场景，通过，无 ASan 报告 | asan-entry.log、asan/results.json |
| 新 TSan 压力入口，sender / release / g++-14，iterations=2 | 86 个完整场景，通过，无数据竞争报告 | tsan-entry.log、tsan/pause_sender_test-exit.txt |
| 新深链入口，仅 chain/value、MaxDepth=1 | 深度 1 的单个子进程通过，未扫描栈极限 | chain-entry.log、chain-smoke/layout-depth1-cases.json |
| 新证据汇总入口，RequireComplete | 预期退出 1；报告 44 个未满足目标，没有接受旧哈希证据 | summary-smoke.log、summary-smoke-check.json |
| 全部 PowerShell / Bash / Python 静态语法检查 | 35 / 16 / 2 个文件，解析错误均为 0 | powershell-parse.json、bash-parse.json、python-parse.json |
| 全量迁移、注释、编码和产物位置检查 | 137 个路径正确；167 个文本程序/配置有测试说明；编码错误和 tests 下产物均为 0 | layout-check.json |
| 旧路径及相对输入人工复核 | 脚本调用、C++ include、编译数据库夹具均正确；日志/输出仍为 build | 迁移清单逐项复核 |
| Git 已跟踪改动的空白检查 | git diff --check 退出 0 | git-diff-check.log |

证据汇总入口使用独立的空 CompletionDirectory，只读取原 EvidenceDirectory 的记录，汇总写入 summary-smoke/evidence-summary.json，原压力汇总未被覆盖。该自检只证明严格拒绝仍然生效，不是一次新的压力验收。

## 生产文件保持一致

- pause.hpp：2595F4FA379B7DD3D524013B8EDF6E930B76795CD5EBD802EE15A6A2A8EE87B9
- pause-branch-design.md：6721FB24166892FE7B96C8D3CB73EB8EDC30A4B49DD36AA12E0D6C7E849FFE96

两项均与本次整理前记录一致；工作区原有其他改动没有重置。

## 覆盖边界

此次仅为目录、注释、路径和编码整理，未重新执行历史 95,100,000 次 Release 压力及相应大型 sanitizer 活动。旧报告继续绑定当时源码哈希；添加注释后哈希不同，不能把旧结果声称为当前源码的新运行结果。

历史归档有故意非法的编辑器夹具、依赖旧 API 的原型、阳性数据竞争探针和有意探查崩溃边界的调试脚本。本次不把它们注册为当前 CTest，也未逐个构建、运行这些历史程序；其工具链、依赖和已知限制见各文件头。

当前正常回归和小量入口验证完成；如需在其他环境复现历史实验，应先核对对应文件头和迁移索引。没有待审批的代码或依赖变更。
