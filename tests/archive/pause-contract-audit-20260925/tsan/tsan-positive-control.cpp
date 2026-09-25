/* 测试项目：TSan 故意数据竞争阳性对照
 * 性质: 检测器验证程序, 刻意包含未同步的共享整数写入, 不是 Pause 产品测试.
 * 项目: 主线程与 writer 同时写同一整数, join 仅等待线程退出, 不消除两个写操作的竞争.
 * 预期: 以 -fsanitize=thread 编译后, TSan 输出 data race 并按 TSAN_OPTIONS 退出 66.
 * 运行: 同目录 run-existing.sh 或 completion 的 tsan-positive/run-positive.sh 编译并检查诊断与退出码.
 * 边界: 仅用于确认检测器能报告已知错误, 不计入产品通过场景数; 不以普通运行返回值判定安全.
 */
#include <thread>
int main()
{
    int intentionally_unsynchronized = 0;
    std::thread writer([&] { intentionally_unsynchronized = 1; });
    intentionally_unsynchronized = 2;
    writer.join();
    return intentionally_unsynchronized == 0;
}
