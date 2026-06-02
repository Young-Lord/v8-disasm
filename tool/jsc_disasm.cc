#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
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

  // Enable V8 bytecode printing
  v8::V8::SetFlagsFromString("--print-bytecode");
  v8::V8::SetFlagsFromString("--print-bytecode-filter=*");

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

    std::string log_path = out_dir + "/" + fname + ".txt";

    // Fork to capture stdout per module (since --print-bytecode goes to stdout)
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
      // Child: redirect stdout to pipe
      close(pipefd[0]);
      dup2(pipefd[1], 1);
      close(pipefd[1]);

      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      if (data.size() >= 32) {
        const uint8_t* hdr = (const uint8_t*)data.data();
        uint32_t magic = hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | (hdr[3]<<24);
        uint32_t payload_len = hdr[20] | (hdr[21]<<8) | (hdr[22]<<16) | (hdr[23]<<24);
        printf("=== Module: %s (magic=0x%08x payload=%u) ===\n\n",
               fname.c_str(), magic, payload_len);
      }

      auto cached = std::make_unique<v8::ScriptCompiler::CachedData>(
          (const uint8_t*)data.data(), (int)data.size(),
          v8::ScriptCompiler::CachedData::BufferNotOwned);

      auto empty = v8::String::NewFromUtf8(isolate, "").ToLocalChecked();
      v8::ScriptOrigin origin(empty);
      v8::ScriptCompiler::Source source(empty, origin, cached.get());

      auto maybe_sfi = v8::ScriptCompiler::CompileUnboundScript(
          isolate, &source, v8::ScriptCompiler::kConsumeCodeCache);

      if (maybe_sfi.IsEmpty()) {
        printf("\n[REJECTED - sanity check or format mismatch]\n");
        _exit(1);
      }

      auto sfi = maybe_sfi.ToLocalChecked();
      auto bound = sfi->BindToCurrentContext();
      auto script = v8::Local<v8::Script>::Cast(bound);
      auto result = script->Run(context);

      printf("\n=== End of module ===\n");
      _exit(0);
    }

    // Parent: read pipe output
    close(pipefd[1]);
    char buf[65536];
    std::string output;
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
      output.append(buf, n);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    // Write output to log file
    std::ofstream log(log_path);
    log << output;
    log.close();

    printf("[%zu] %s: %s (%zu bytes)\n",
           i, fname.c_str(), ok ? "OK" : "REJECTED", output.size());

    if (ok) success++;
    else fail++;
    fflush(stdout);
  }

  printf("\n=== Summary: %d success, %d failed out of %zu ===\n",
         success, fail, files.size());

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return fail > 0 ? 1 : 0;
}
