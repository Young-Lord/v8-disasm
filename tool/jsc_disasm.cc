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

static std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return "";
  size_t sz = f.tellg(); f.seekg(0);
  std::string d(sz, '\0'); f.read(&d[0], sz);
  return d;
}

static std::vector<std::string> ListJSC(const std::string& dir) {
  std::vector<std::string> files;
  DIR* d = opendir(dir.c_str());
  if (!d) return files;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size()-4) == ".jsc") files.push_back(n);
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

  std::string mod_dir = argv[1];
  std::string out_dir = argv[2];

  // Enable print-bytecode BEFORE init
  v8::V8::SetFlagsFromString("--print-bytecode");
  v8::V8::SetFlagsFromString("--print-bytecode-filter=*");
  v8::V8::SetFlagsFromString("--single-threaded");

  fprintf(stderr, "Step 1: Init ICU...\n");
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  fprintf(stderr, "Step 2: Create platform...\n");
  auto platform = v8::platform::NewDefaultPlatform();
  fprintf(stderr, "Step 3: Init platform...\n");
  v8::V8::InitializePlatform(platform.get());
  fprintf(stderr, "Step 4: Init V8...\n");
  v8::V8::Initialize();
  fprintf(stderr, "Step 5: Create isolate...\n");

  v8::Isolate::CreateParams cp;
  cp.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* iso = v8::Isolate::New(cp);

  auto files = ListJSC(mod_dir);
  fprintf(stderr, "=== Found %zu .jsc files ===\n", files.size());

  for (size_t i = 0; i < files.size(); i++) {
    std::string data = ReadFile(mod_dir + "/" + files[i]);
    if (data.empty()) {
      fprintf(stderr, "[%zu] %s: EMPTY\n", i, files[i].c_str());
      continue;
    }

    // Log per-module to file
    std::string log = out_dir + "/" + files[i] + ".txt";

    v8::Isolate::Scope iscope(iso);
    v8::HandleScope hscope(iso);
    v8::Local<v8::Context> ctx = v8::Context::New(iso);
    v8::Context::Scope cscope(ctx);

    // Get header info
    if (data.size() >= 32) {
      auto hdr = (const uint8_t*)data.data();
      uint32_t magic = hdr[0]|(hdr[1]<<8)|(hdr[2]<<16)|(hdr[3]<<24);
      uint32_t plen = hdr[20]|(hdr[21]<<8)|(hdr[22]<<16)|(hdr[23]<<24);
      fprintf(stderr, "[%zu] %s: magic=0x%08x payload=%u\n",
              i, files[i].c_str(), magic, plen);
    }

    // Redirect stdout to file to capture --print-bytecode output
    fflush(stdout);
    int old_stdout = dup(1);
    FILE* lf = fopen(log.c_str(), "w");
    dup2(fileno(lf), 1);

    auto cached = std::make_unique<v8::ScriptCompiler::CachedData>(
        (const uint8_t*)data.data(), (int)data.size(),
        v8::ScriptCompiler::CachedData::BufferNotOwned);

    auto src = v8::String::NewFromUtf8(iso, "").ToLocalChecked();
    v8::ScriptOrigin origin(src.As<v8::Value>());
    v8::ScriptCompiler::Source source(src, origin, cached.get());

    auto m = v8::ScriptCompiler::CompileUnboundScript(
        iso, &source, v8::ScriptCompiler::kConsumeCodeCache);

    if (!m.IsEmpty()) {
      auto sfi = m.ToLocalChecked();
      auto bound = sfi->BindToCurrentContext();
      auto script = v8::Local<v8::Script>::Cast(bound);
      (void)script->Run(ctx);
      fprintf(stderr, "[%zu] %s: OK\n", i, files[i].c_str());
    } else {
      fprintf(stderr, "[%zu] %s: REJECTED\n", i, files[i].c_str());
    }

    // Restore stdout
    fflush(stdout);
    dup2(old_stdout, 1);
    close(old_stdout);
    fclose(lf);
  }

  iso->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return 0;
}
