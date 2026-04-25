#if defined(_WIN32) || defined(_WIN64)
	#include <windows/pyconfig.h>
# elif defined(__linux__)
	#if defined(__x86_64__) && defined(__LP64__)
	#include <linux/x86_64/pyconfig.h>
	#elif defined(__i386__)
	#include <linux/i386/pyconfig.h>
	#endif
#else
	#error "Unsupported platform"
#endif
