#include_next <assert.h>
/* IDF may evaluate NDEBUG assertions; SQLite references DEBUG-only names.
 * Restore standard NDEBUG semantics for the upstream library only. */
#ifdef NDEBUG
#undef assert
#define assert(expression) ((void)0)
#endif
