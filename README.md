# cloudgame_checkin

使用 C++ 编写的云原神每日奖励领取工具，只需要配置账号密码，不用手动获取或更新token。

本项目来源于个人逆向分析与编程学习，主要用于研究云原神登录流程、会话保持、接口请求及极验4（GeeTest captcha v4）滑块验证流程。项目仅提供基础命令行功能，不包含图形界面。

## 功能概览

- 使用账号和密码完成登录
- 自动保存并复用本地会话
- 查询当前免费时长、畅玩卡状态和原点余额
- 检查并领取每日登录奖励
- 处理登录过程中可能出现的GeeTest v4滑块验证流程
- 输出清晰的运行日志和错误信息

## 环境要求

- CMake 3.15 或更高版本
- 支持 C++17 的编译器
- OpenSSL
- nlohmann/json
- cpp-httplib

项目中已经包含 `stb_image.h`，无需单独安装。

## 编译

### 使用 vcpkg 安装依赖

```bash
vcpkg install openssl nlohmann-json cpp-httplib
```

### 生成并编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

使用 vcpkg 时，请根据本机环境补充 CMake Toolchain 参数：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

## 使用方法

程序支持两种运行方式：

```text
cloudgame_checkin
cloudgame_checkin <账号> <密码>
```

### 指定账号运行

首次添加账号，或者只想运行某一个账号时，传入账号和密码：

```bash
cloudgame_checkin <账号> <密码>
```

Windows 示例：

```powershell
.\build\Release\cloudgame_checkin.exe "your_account" "your_password"
```

Linux 示例：

```bash
./build/cloudgame_checkin "your_account" "your_password"
```

程序只会查找当前账号对应的缓存文件，不会读取或尝试其他账号的缓存。

- 没有该账号缓存：执行完整登录流程，成功后创建缓存。
- 账号和缓存中的密码一致：优先尝试复用已有会话。
- 缓存会话已失效：使用缓存中的设备信息重新进行账号密码登录。
- 账号相同但密码不同：不会使用旧 Cookie、Token 或旧会话，而是直接执行完整登录，并在成功后用新密码和新会话覆盖该账号缓存。

因此修改密码后，只需要重新执行：

```bash
cloudgame_checkin <账号> <新密码>
```

### 无参数运行全部缓存账号

当已经至少成功登录并缓存过一个账号后，可以不带任何参数运行：

Windows：

```powershell
.\build\Release\cloudgame_checkin.exe
```

Linux：

```bash
./build/cloudgame_checkin
```

程序会扫描缓存目录中的全部有效账号缓存，读取其中保存的账号和密码，并依次完成登录/会话恢复和每日奖励领取。

## 项目结构

```text
.
├── CMakeLists.txt
├── main.cpp
├── offset_estimator.hpp
└── stb_image.h
```

## 注意事项

- 请妥善保管账号、密码及本地会话文件。
- 不建议在公共电脑或不可信环境中运行。
- 平台接口、验证方式或返回数据发生变化后，项目可能无法正常工作。
- 本项目没有提供任何稳定性保证，请自行评估使用风险。

## 免责声明

本项目仅用于个人学习、技术研究和交流，不用于商业用途，也不鼓励任何形式的接口滥用、批量操作或绕过平台限制。

项目与相关游戏平台及其运营方不存在任何隶属、授权或合作关系。使用者应遵守所在地区的法律法规、平台用户协议及相关规则。因使用本项目造成的账号异常、数据损失或其他后果，由使用者自行承担。
