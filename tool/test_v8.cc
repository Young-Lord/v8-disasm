#include <cstdio>
#include <libplatform/libplatform.h>
#include <v8.h>

int main() {
  fprintf(stderr, "1\n");
  v8::V8::SetFlagsFromString("--print-bytecode");
  fprintf(stderr, "2\n");
  v8::V8::InitializeICUDefaultLocation("");
  fprintf(stderr, "3\n");
  auto p = v8::platform::NewDefaultPlatform();
  fprintf(stderr, "4\n");
  v8::V8::InitializePlatform(p.get());
  fprintf(stderr, "5\n");
  v8::V8::Initialize();
  fprintf(stderr, "6\n");

  auto cp = v8::Isolate::CreateParams();
  cp.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  auto iso = v8::Isolate::New(cp);
  fprintf(stderr, "7\n");
  iso->Dispose();
  fprintf(stderr, "8\n");

  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  fprintf(stderr, "DONE\n");
  return 0;
}
