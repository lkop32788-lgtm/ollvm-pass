# ollvm-pass — Windows 内核驱动 LLVM 混淆套件

基于 **LLVM New Pass Manager** 的 Windows 10/11 内核驱动（`.sys`）混淆插件，
面向 clang-cl + MSVC 工具链。包含六大核心混淆模块，保持驱动功能完整的前提下
有效提升逆向难度、规避杀软静态特征检测。

---

## 目录结构

```
ollvm-pass/
├── CMakeLists.txt                     # 项目构建脚本
├── include/
│   └── Obfuscation/
│       ├── Utils.h                    # 公共工具（PRNG、密码原语、谓词）
│       ├── ControlFlowFlattening.h    # Pass 1 — 控制流平坦化
│       ├── BogusControlFlow.h         # Pass 2 — 虚假控制流
│       ├── InstructionSubstitution.h  # Pass 3 — 指令替换
│       ├── StringEncryption.h         # Pass 4 — 字符串加密
│       ├── ConstantEncryption.h       # Pass 5 — 常量混淆
│       └── IndirectBranching.h        # Pass 6 — 间接调用混淆
├── src/
│   ├── ObfuscationPlugin.cpp          # LLVM 插件入口 & Pass 注册
│   ├── ControlFlowFlattening.cpp
│   ├── BogusControlFlow.cpp
│   ├── InstructionSubstitution.cpp
│   ├── StringEncryption.cpp
│   ├── ConstantEncryption.cpp
│   └── IndirectBranching.cpp
└── test/
    ├── test_stub.h                    # 跨平台 WDK 类型存根（CI 用）
    ├── test_driver.cpp                # 最小化 Windows 内核驱动示例
    ├── test_harness.cpp               # 用户态功能验证测试套件
    └── CMakeLists.txt
```

---

## 六大核心混淆模块

### Pass 1 — 控制流平坦化（Control Flow Flattening，`cff`）

将函数体转换为单一的 `switch-dispatch` 循环，使原始控制流边在静态分析中不可见：

```
entry → loop_header(switch(state)) → case_0 → loop_header
                                   → case_1 → loop_header
                                   → ...
```

- 每个基本块分配随机 case 编号（32-bit 随机整数）
- 消除所有 PHI 节点，替换为栈上临时变量
- 对逆向工程及自动化 CFG 重建有显著干扰效果

### Pass 2 — 虚假控制流（Bogus Control Flow，`bcf`）

在每个基本块前插入"永远为真"的不透明谓词，同时生成一个永远不被执行的死克隆块作为假分支：

```
before:   BB → succ
after:    BB_head → opaque_true? → BB_tail
                                 → junk_clone (never executed)
```

不透明谓词基于数学恒等式 `(x*(x+1)) % 2 == 0`，常量折叠无法消除。

### Pass 3 — 指令替换（Instruction Substitution，`isub`）

将常见整数运算替换为语义等价但更复杂的指令序列：

| 原始指令      | 替换结果                          |
|------------|-------------------------------|
| `a + b`    | `a - (~b) - 1`                |
| `a - b`    | `a + (~b) + 1`                |
| `a & b`    | `~(~a \| ~b)` (De Morgan)     |
| `a \| b`   | `~(~a & ~b)` (De Morgan)      |
| `a ^ b`    | `(a \| b) & ~(a & b)`         |

应用概率约 75%，兼顾混淆强度与代码体积。

### Pass 4 — 字符串加密（String Encryption，`strenc`）

- 定位模块内所有字符串字面量（`i8[]` / `i16[]` 空终止）
- 使用滚动 XOR 加密（64-bit 密钥，每个字符串独立密钥）
- 加密后的字节数组存入 `.ollvm$str` 节
- 在每个使用点前内联注入解密存根，将结果写入**栈缓冲区**

**内核驱动安全保证**：
- 不分配堆内存（无 `ExAllocatePool`）
- 使用 `alloca`，任何 IRQL 均安全
- 同一明文字符串→不同密文，对抗内存扫描特征匹配

### Pass 5 — 常量混淆（Constant Encryption，`cenc`）

将宽于 8-bit 的整数立即数替换为运行时计算表达式：

- **XOR 变体**：`(C ^ M1 ^ M2) ^ M2 ^ M1` — 多层掩码异或
- **Add/Sub 变体**：`(C + K) - K` — 随机偏移加减对

两种变体按随机选择交替应用，防止被模式识别。

### Pass 6 — 间接调用混淆（Indirect Branching，`ibc`）

将模块内部函数的直接调用转换为通过加密函数指针表的间接调用：

```
before:  call @foo(args)
after:   %enc  = load @.ollvm.disp.foo
         %real = xor %enc, KEY
         %fptr = inttoptr %real to FooTy*
         call %fptr(args)
```

- 函数指针在编译期加密存入 `.ollvm$iat` 节
- 完全打断静态调用图，对抗基于调用模式的特征检测

---

## 构建步骤

### 前提条件

| 工具 | 最低版本 |
|------|---------|
| CMake | 3.20 |
| LLVM/Clang | 15.0（推荐 17+） |
| Visual Studio / Build Tools | 2022 |
| Windows SDK | 10.0.22000 或以上 |
| WDK | 对应 SDK 版本 |

### 配置与编译

```powershell
# 在 x64 Native Tools Command Prompt for VS 2022 中运行

git clone https://github.com/lkop32788-lgtm/ollvm-pass.git
cd ollvm-pass

cmake -B build -G "Ninja" ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DLLVM_DIR="C:/Program Files/LLVM/lib/cmake/llvm"

cmake --build build --config Release
```

编译产物：`build/OllvmPass.dll`

---

## 使用方法

### 单独启用某个 Pass

```powershell
clang-cl -O2 -target x86_64-pc-windows-msvc  ^
         -fpass-plugin=build/OllvmPass.dll    ^
         -mllvm -passes="cff"                 ^
         /kernel /GS- driver.c -o driver.sys
```

### 启用全部 Pass（推荐）

```powershell
clang-cl -O2 -target x86_64-pc-windows-msvc  ^
         -fpass-plugin=build/OllvmPass.dll    ^
         -mllvm -passes="ollvm-all"            ^
         /kernel /GS- /GL- /W3                ^
         driver.c -o driver.sys
```

### Pass 名称速查表

| Pass 名称   | 对应模块                  | 级别     |
|------------|--------------------------|--------|
| `cff`      | 控制流平坦化               | 函数级   |
| `bcf`      | 虚假控制流                 | 函数级   |
| `isub`     | 指令替换                   | 函数级   |
| `cenc`     | 常量混淆                   | 函数级   |
| `strenc`   | 字符串加密                 | 模块级   |
| `ibc`      | 间接调用混淆               | 模块级   |
| `ollvm-all`| 以上全部（推荐顺序自动组合） | 模块级   |

---

## 功能验证测试

测试套件可在无 WDK / 无 Windows 环境下运行（使用 `test_stub.h` 模拟 WDK 类型）：

```bash
cd test
g++ -std=c++17 -I../include test_harness.cpp test_driver.cpp -o harness
./harness
```

预期输出：

```
[PASS] Echo basic (status)
[PASS] Echo basic (bytes written)
[PASS] Echo basic (content)
[PASS] Echo magic (status)
[PASS] Echo magic (bytes written)
[PASS] Echo magic (first byte = 'M')
[PASS] Bad IOCTL code
[PASS] Null input
[PASS] Buffer too small

9 passed, 0 failed.
All tests passed.
```

---

## 免杀增强设计要点

1. **每 pass 独立随机密钥**：基于模块标识符 + 函数指针混合哈希，确保同一源文件每次编译产生不同混淆结果（需配合随机化种子选项）。
2. **不透明谓词**：采用数学恒等式而非外部随机源，避免在内核态引入不确定性。
3. **栈内解密**：字符串加密的解密结果仅存在于栈上，不修改 `.rdata` 段，对抗内存扫描。
4. **分节存储**：加密数据放在命名节（`.ollvm$str`、`.ollvm$iat`），可通过链接脚本进一步处理（如二次加密、随机化节名）。
5. **选择性应用**：所有 Pass 检查 `shouldObfuscate()` 谓词，跳过声明、内置函数及带 `noinline`/`nollvm` 注解的函数，避免破坏关键内核路径。

---

## 注意事项

- 混淆会增加代码体积和执行时间；建议对热路径函数使用 `__attribute__((annotate("nollvm")))` 豁免。
- WDM / KMDF 驱动需在 `/GS-`（禁用安全检查）下编译，与 Pass 兼容。
- 不支持 C++ 异常（与内核驱动要求一致）。
- 如需与 WDK 的 `/GL`（整体程序优化）同时使用，请将 `ollvm-all` 插入 `OptimizerLastEP`（默认已配置）。
