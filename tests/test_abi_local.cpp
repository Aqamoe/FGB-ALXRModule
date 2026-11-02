// Local 产物的 ABI 冒烟（普通测试，进 ctest）。
//
// Kit 的 SMOKE_SOURCE 会创建固定名 smoke-test 目标，一仓两个 fgb_add_module 会撞名，
// 所以 remote 拿 smoke-test，local 的 ABI 校验写在这里：动态加载本仓库构建出的
// alxr-local.dll（与本 exe 同目录），走真实导出符号。
// 为什么不进 smoke-test：完整端到端还需要厂商 alxr_engine.dll + 真实 OpenXR 运行时，
// CI 上必然缺依赖；这里覆盖「导出、ABI 协商、缺依赖时优雅失败」这三段可在 CI 验证的部分。

#include "alxr_abi.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb_module_api.h"

#include <windows.h>

#include <string>

namespace {

std::string ExeDirectory() {
    char exe_path[MAX_PATH] = {0};
    ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string directory(exe_path);
    const std::size_t slash = directory.find_last_of("\\/");
    if (slash != std::string::npos) {
        directory = directory.substr(0, slash + 1);
    } else {
        directory = ".\\";
    }
    return directory;
}

std::string ModuleDllPath() { return ExeDirectory() + "alxr-local.dll"; }

bool FileExists(const std::string& path) {
    const DWORD attributes = ::GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

} // namespace

int main(int argc, char** argv) {
    // 冒烟风格也支持显式传路径（本地手动跑时用）。
    const std::string dll_path = argc > 1 ? argv[1] : ModuleDllPath();

    HMODULE library = ::LoadLibraryExA(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败 %s（错误码 %lu，alxr-local.dll 是否已构建？）\n",
                    dll_path.c_str(), ::GetLastError());
        return CheckSummary("alxr-local-abi");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module != nullptr) {
        CHECK(module->id == std::string{"com.fgbbridge.alxr-local"});
        CHECK(module->display_name == std::string{"ALXR (OpenXR Local)"});
        CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
        CHECK_EQ(module->capabilities,
                 static_cast<std::uint32_t>(FGB_CAP_EXPRESSION | FGB_CAP_EYES));
        CHECK_EQ(module->struct_size, static_cast<std::uint32_t>(sizeof(FgbModule)));
    }
    // ABI 协商：版本不符必须返回 NULL，宿主据此报「ABI 版本不兼容」。
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr);
    CHECK(query(0) == nullptr);

    if (module != nullptr && module->initialize != nullptr && module->teardown != nullptr) {
        fgb::kit::testing::HostMock host;
        // module_dir 用 DLL/exe 所在目录：与真实宿主「把模块目录交给模块」的语义一致，
        // Kit 的 Library 会先找 module_dir，再找系统目录与 PATH。
        const std::string exe_dir = ExeDirectory();
        FgbInitParams params = host.MakeParams(exe_dir);
        void* self = nullptr;

        // 厂商依赖的存在与否都必须是确定行为（不允许未定义崩溃）。
        // 只检查模块目录旁边有没有 alxr_engine.dll：CI 与常规开发机都没有，
        // 走「优雅失败」断言；装了厂商 DLL 的机器走「不崩即可」断言。
        // （系统目录/PATH 命中会漏判成失败分支，但那也只是少测成功路径，不会误报失败。）
        const bool engine_available = FileExists(exe_dir + alxr::kEngineDllName);

        const int initialized = module->initialize(&self, host.services(), &params);
        if (engine_available) {
            // 真装了 libalxr 的开发机：允许成功；失败也只应是 OpenXR 会话原因，不崩即可。
            CHECK(initialized == 1 || initialized == 0);
            if (initialized == 1 && self != nullptr) {
                module->update(self); // 允许起不来会话，只要求不崩、不提交脏帧
                module->teardown(self);
            }
        } else {
            // CI / 未装依赖：缺 alxr_engine.dll 必须优雅失败，状态里给可操作提示。
            CHECK_EQ(initialized, 0);
            CHECK(self == nullptr);
            bool mentioned_engine = false;
            for (const auto& status : host.statuses()) {
                if (status.first == FGB_STATUS_ERROR &&
                    status.second.find(alxr::kEngineDllName) != std::string::npos) {
                    mentioned_engine = true;
                }
            }
            CHECK(mentioned_engine);
        }
    }

    ::FreeLibrary(library);
    return CheckSummary("alxr-local-abi");
}
