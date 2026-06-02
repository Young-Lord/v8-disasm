// jsc_disasm.cc - Load .jsc (V8 bytecode) and disassemble with --print-bytecode
// Compile: g++ -std=c++17 -I v8/include jsc_disasm.cc -o jsc_disasm \
//          -L v8/out/release -lv8_monolith -pthread -licui18n -licuuc
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <libplatform/libplatform.h>
#include <v8.h>

class JSCDisassembler {
public:
  JSCDisassembler(const std::string& module_dir, const std::string& out_dir)
      : module_dir_(module_dir), out_dir_(out_dir) {}

  bool Init() {
    v8::V8::InitializeICUDefaultLocation("");
    platform_ = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform_.get());
    v8::V8::Initialize();

    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    isolate_ = v8::Isolate::New(create_params);
    return isolate_ != nullptr;
  }

  void Run() {
    auto files = ListJSCFiles();
    printf("Found %zu .jsc files\n", files.size());

    for (size_t i = 0; i < files.size(); i++) {
      auto& f = files[i];
      printf("[%zu/%zu] Processing %s ... ", i + 1, files.size(), f.c_str());
      fflush(stdout);

      std::string data = ReadFile(module_dir_ + "/" + f);
      if (data.empty()) {
        printf("SKIP (empty)\n");
        continue;
      }

      v8::Isolate::Scope isolate_scope(isolate_);
      v8::HandleScope handle_scope(isolate_);
      v8::Local<v8::Context> context = v8::Context::New(isolate_);
      v8::Context::Scope context_scope(context);

      // Create cached data from the .jsc content
      // Note: .jsc has 32-byte SerializedCodeData header, then bytecode
      auto cached_data = std::make_unique<v8::ScriptCompiler::CachedData>(
          reinterpret_cast<const uint8_t*>(data.data()),
          static_cast<int>(data.size()),
          v8::ScriptCompiler::CachedData::BufferNotOwned);

      // Create source with cached data
      v8::Local<v8::String> source_code =
          v8::String::NewFromUtf8(isolate_, "").ToLocalChecked();
      v8::ScriptOrigin origin(isolate_, source_code);
      v8::ScriptCompiler::Source source(source_code, origin, cached_data.get());

      // Try to compile with consumed cached data
      auto result = v8::ScriptCompiler::CompileUnboundScript(
          isolate_, &source, v8::ScriptCompiler::kConsumeCodeCache);

      if (result.IsEmpty()) {
        printf("FAILED (rejected)\n");
        continue;
      }

      // Bound the script to run it
      auto bound = result.ToLocalChecked()->BindToCurrentContext();
      auto script = v8::Local<v8::Script>::Cast(bound);

      // Try running to see exports  
      auto run_result = script->Run(context);
      if (!run_result.IsEmpty()) {
        // Get global object to see what was exported
        auto global = context->Global();
        auto prop_names = global->GetOwnPropertyNames(context).ToLocalChecked();
        printf("OK (%d exports)\n", prop_names->Length());
      } else {
        printf("OK\n");
      }

      // Output file name
      std::string base_name = f;
      auto dot = base_name.rfind('.');
      if (dot != std::string::npos) base_name = base_name.substr(0, dot);

      // Save source file name map
      std::string info_path = out_dir_ + "/" + base_name + ".info";
      std::ofstream info_file(info_path);
      if (info_file) {
        if (!result.IsEmpty()) {
          auto sfi = result.ToLocalChecked();
          // We can also try to get script info
        }
        info_file.close();
      }
    }
  }

  void Shutdown() {
    if (isolate_) isolate_->Dispose();
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
  }

private:
  std::vector<std::string> ListJSCFiles() {
    std::vector<std::string> files;
    DIR* dir = opendir(module_dir_.c_str());
    if (!dir) return files;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name.size() > 4 && name.substr(name.size() - 4) == ".jsc") {
        files.push_back(name);
      }
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
  }

  std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return "";
    size_t size = file.tellg();
    file.seekg(0);
    std::string data(size, '\0');
    file.read(&data[0], size);
    return data;
  }

  std::string module_dir_;
  std::string out_dir_;
  std::unique_ptr<v8::Platform> platform_;
  v8::Isolate* isolate_ = nullptr;
};

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <modules_dir> <output_dir>\n", argv[0]);
    return 1;
  }

  JSCDisassembler disasm(argv[1], argv[2]);
  if (!disasm.Init()) {
    fprintf(stderr, "Failed to initialize V8\n");
    return 1;
  }

  disasm.Run();
  disasm.Shutdown();
  return 0;
}
