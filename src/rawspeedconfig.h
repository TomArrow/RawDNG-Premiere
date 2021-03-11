#pragma once

#if defined(__SSE2__)
#define WITH_SSE2
#else
/* #undef WITH_SSE2 */
#endif

#define HAVE_PUGIXML

#define HAVE_OPENMP

#define HAVE_PUGIXML

#define HAVE_ZLIB

#define HAVE_JPEG
#define HAVE_JPEG_MEM_SRC

#define HAVE_CXX_THREAD_LOCAL
#define HAVE_GCC_THREAD_LOCAL

// which aligned memory allocation function is available, if any?
// only the first one found will be enabled
#define HAVE_POSIX_MEMALIGN
#define HAVE_ALIGNED_ALLOC
#define HAVE_MM_MALLOC
#define HAVE_ALIGNED_MALLOC

//#define RAWSPEED_STANDALONE_BUILD
#ifdef RAWSPEED_STANDALONE_BUILD
#define RAWSPEED_SOURCE_DIR "@RAWSPEED_SOURCE_DIR@"
#else
// If rawspeed is being built as part of some larger build, we can not retain
// the RAWSPEED_SOURCE_DIR, because that would affect the reproducible builds.
#endif

#undef OPENMP_FIRSTPRIVATE_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
#undef OPENMP_SHARED_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES

#ifdef HAVE_OPENMP
#define OPENMP_FIRSTPRIVATE_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
#ifdef OPENMP_FIRSTPRIVATE_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
// See https://bugs.llvm.org/show_bug.cgi?id=35873
//     https://redmine.darktable.org/issues/12568
#define OMPFIRSTPRIVATECLAUSE(...)
#else
#define OMPFIRSTPRIVATECLAUSE(...) firstprivate(__VA_ARGS__)
#endif
#undef OPENMP_FIRSTPRIVATE_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES

#define OPENMP_SHARED_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
#ifdef OPENMP_SHARED_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
// See https://godbolt.org/z/AiyuX9
#define OMPSHAREDCLAUSE(...)
#else
#define OMPSHAREDCLAUSE(...) shared(__VA_ARGS__)
#endif
#undef OPENMP_SHARED_CLAUSE_IS_BROKEN_FOR_CONST_VARIABLES
#endif // HAVE_OPENMP

// see http://clang.llvm.org/docs/LanguageExtensions.html
#ifndef __has_feature      // Optional of course.
#define __has_feature(x) 0 // Compatibility with non-clang compilers.
#endif
#ifndef __has_extension
#define __has_extension __has_feature // Compatibility with pre-3.0 compilers.
#endif

#define RAWSPEED_UNLIKELY_FUNCTION __attribute__((cold))
#define RAWSPEED_NOINLINE __attribute__((noinline))




#ifdef _MSC_VER
#define __attribute__(x) 
#define __builtin_unreachable(x)
//#define assert(x)
#define ASAN_REGION_IS_POISONED(x) 0
#define __PRETTY_FUNCTION__ __FUNCSIG__
#else

#endif
