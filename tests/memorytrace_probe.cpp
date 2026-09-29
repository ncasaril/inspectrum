#include "memorytrace.h"

struct ProbeSource { virtual ~ProbeSource() = default; };

int main()
{
    ProbeSource source;
    MemoryTrace::request("small", &source, 0, 1, 8);
    MemoryTrace::request("large", &source, 123, 262144, 2097152);
}
