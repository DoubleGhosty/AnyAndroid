// Android's systrace (<cutils/trace.h>), as SkATrace uses it: never enabled.
#pragma once

#define ATRACE_TAG_VIEW 0
#define ATRACE_ENABLED() 0
#define ATRACE_BEGIN(name) ((void)(name))
#define ATRACE_END() ((void)0)
#ifndef CC_UNLIKELY
#define CC_UNLIKELY(x) (x)
#endif
