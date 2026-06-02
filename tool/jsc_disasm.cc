// jsc_disasm.cc - Load .jsc (V8 SerializedCodeData) and disassemble with --print-bytecode
// Compile: g++ -std=c++17 -I v8/include jsc_disasm.cc -o jsc_disasm \
//          -L v8/out/release -lv8_monolith -pthread -licui18n -licuuc -licudata
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <libplatform/libplatform.h>
#include <v8.h>

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return "";
  size_t size = file.tellg();
  file.seekg(0);
  std::string data(size, '\0');
  file.read(&data[0], size);
  return data;
}

std::vector<std::string> ListJSCFiles(const std::string& dir) {
  std::vector<std::string> files;
  DIR* d = opendir(dir.c_str());
  if (!d) return files;
  struct dirent* entry;
  while ((entry = readdir(d)) != nullptr) {
    std::string name = entry->d_name;
    if (name.size() > 4 && name.substr(name.size() - 4) == ".jsc")
      files.push_back(name);
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  return files;
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <modules_dir> <output_dir>\n", argv[0]);
    return 1;
  }

  std::string module_dir = argv[1];
  std::string out_dir = argv[2];

  // Enable V8 flags for bytecode printing
  v8::V8::SetFlagsFromString("--print-bytecode");
  v8::V8::SetFlagsFromString("--print-bytecode-filter=*");
  v8::V8::SetFlagsFromString("--trace-deserialization");

  v8::V8::InitializeICUDefaultLocation(argv[0]);
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);

  auto files = ListJSCFiles(module_dir);
  printf("=== Found %zu .jsc files ===\n\n", files.size());

  int success = 0, fail = 0;
  for (size_t i = 0; i < files.size(); i++) {
    auto& fname = files[i];
    std::string data = ReadFile(module_dir + "/" + fname);
    if (data.empty()) {
      printf("[%zu] %s: EMPTY\n", i, fname.c_str());
      continue;
    }

    // Redirect stdout to capture bytecode output
    // We'll use a pipe approach - redirect to log file per module
    std::string log_path = out_dir + "/" + fname + ".log";

    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    // Read header for info
    if (data.size() >= 32) {
      const uint8_t* hdr = (const uint8_t*)data.data();
      uint32_t magic = hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | (hdr[3]<<24);
      uint32_t vhash = hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | (hdr[7]<<24);
      uint32_t payload_len = hdr[20] | (hdr[21]<<8) | (hdr[22]<<16) | (hdr[23]<<24);
      printf("[%zu] %s: magic=0x%08x vhash=0x%08x payload=%u ",
             i, fname.c_str(), magic, vhash, payload_len);
    }

    auto cached_data = std::make_unique<v8::ScriptCompiler::CachedData>(
        (const uint8_t*)data.data(), (int)data.size(),
        v8::ScriptCompiler::CachedData::BufferNotOwned);

    auto source_str = v8::String::NewFromUtf8(isolate, "").ToLocalChecked();
    v8::ScriptOrigin origin(isolate, source_str);
    v8::ScriptCompiler::Source source(source_str, origin, cached_data.get());

    auto maybe_sfi = v8::ScriptCompiler::CompileUnboundScript(
        isolate, &source, v8::ScriptCompiler::kConsumeCodeCache);

    if (maybe_sfi.IsEmpty()) {
      // Try without cached data (as regular empty script)
      printf("BYTECODE-REJECTED\n");
      fail++;
    } else {
      auto sfi = maybe_sfi.ToLocalChecked();
      auto bound = sfi->BindToCurrentContext();
      auto script = v8::Local<v8::Script>::Cast(bound);

      auto result = script->Run(context);
      if (!result.IsEmpty()) {
        auto global = context->Global();
        auto props = global->GetOwnPropertyNames(context).ToLocalChecked();
        printf("OK (%d exports)\n", props->Length());
      } else {
        printf("OK (no exports)\n");
      }
      success++;
    }
  }

  printf("\n=== Summary: %d success, %d failed out of %zu ===\n",
         success, fail, files.size());

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return 0;
}
