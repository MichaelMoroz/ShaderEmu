// A compiler crash found building Tiberian Dawn (gpu_vm/userland/apps/README.md, "Compiler bugs"): with debug
// information (-g or -gline-tables-only, at any -O), the deleting destructor of an abstract class (which clang emits
// as a body of `unreachable` alone: a function with no instruction) crashes the gpu_vm assembly printer in
// DwarfDebug::endFunctionImpl (MCSymbol::getSection: "isInSection() && Invalid accessor!"); likely the function's end label,
// a soft label placed after the open bundle, is never put in a section when no bundle was made (a C function of
// __builtin_unreachable() alone does not crash).
// Vanilla Conquer's CMake builds Release with -g.
//   clang++ --target=gpuvm-unknown-linux -O2 -g -c dtor_debug.cpp     -> assertion, crash
//   clang++ --target=gpuvm-unknown-linux -O2 -c dtor_debug.cpp        -> fine
class K {
public:
    K();
    virtual ~K();
    virtual void f() = 0;
};
K::K() {}
K::~K() {}
