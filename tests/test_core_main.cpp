#include "TestSupport.h"

void RunCacheTests();
void RunDepthOpsTests();
void RunRuntimePathResolverTests();
void RunTilerTests();

int main() {
  InitializeTestProcess();
  RunCacheTests();
  RunDepthOpsTests();
  RunRuntimePathResolverTests();
  RunTilerTests();
  return 0;
}
