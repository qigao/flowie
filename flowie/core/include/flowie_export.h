#ifndef FLOWIE_EXPORT_H
#define FLOWIE_EXPORT_H

#ifndef FLOWIE_API
  #if defined(_WIN32) || defined(__CYGWIN__)
    #if defined(FLOWIE_BUILD)
      #define FLOWIE_API __declspec(dllexport)
    #else
      #define FLOWIE_API __declspec(dllimport)
    #endif
  #elif defined(__GNUC__) && __GNUC__ >= 4
    #define FLOWIE_API __attribute__((visibility("default")))
  #else
    #define FLOWIE_API
  #endif
#endif

#ifdef __cplusplus
  #define FLOWIE_C_API extern "C" FLOWIE_API
  #define FLOWIE_INTERNAL_C_API extern "C"
#else
  #define FLOWIE_C_API FLOWIE_API
  #define FLOWIE_INTERNAL_C_API
#endif

#endif /* FLOWIE_EXPORT_H */
