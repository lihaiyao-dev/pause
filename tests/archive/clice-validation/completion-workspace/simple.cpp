/* 测试项目：clice-validation/completion-workspace/simple.cpp
 * clice 补全夹具: Example 对象成员 value 前的 U+00A7 是光标标记.
 * 入口: clice inspect/补全工具读取; U+00A7 刻意使其不是可直接编译的 C++ 程序, 不加入构建.
 * 预期: 光标处包含 value; 需要支持该标记的历史 clice, 不涉及 Pause 行为.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
struct Example { int value; }; int test(Example example) { return example.§value; }