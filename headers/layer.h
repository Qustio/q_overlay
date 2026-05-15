#ifndef LAYER
#define LAYER

#ifdef _WIN32
	#include <Windows.h>
#endif

#undef EXPORT
#ifdef _WIN32
	#define EXPORT __declspec(dllexport)
#else
	#define EXPORT
#endif

EXPORT int aboba(int);

#endif // LAYERE490: No fold found
