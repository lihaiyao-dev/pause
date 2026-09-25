/* 测试项目：resource-review/tsan_probe.cpp
 * TSan 最小链接/启动探针, main 只返回 0, 不创建线程或访问 Pause.
 * 入口: 手动用历史 clang++ -fsanitize=thread 编译后运行; 无 CLI 参数.
 * 预期: 正常退出 0; 只证明运行时可启动, 不证明 TSan 可检测数据竞争.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
int main() { return 0; }
