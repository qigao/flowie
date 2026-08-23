#ifndef FLOWIE_PROTOCOL_EXPORT_H
#define FLOWIE_PROTOCOL_EXPORT_H

#ifndef FLOWIE_PROTOCOL_API
  #if defined(_WIN32) || defined(__CYGWIN__)
    #if defined(FLOWIE_PROTOCOL_BUILD)
      #define FLOWIE_PROTOCOL_API __declspec(dllexport)
    #else
      #define FLOWIE_PROTOCOL_API __declspec(dllimport)
    #endif
  #elif defined(__GNUC__) && __GNUC__ >= 4
    #define FLOWIE_PROTOCOL_API __attribute__((visibility("default")))
  #else
    #define FLOWIE_PROTOCOL_API
  #endif
#endif

#ifdef __cplusplus
  #define FLOWIE_PROTOCOL_C_API extern "C" FLOWIE_PROTOCOL_API
#else
  #define FLOWIE_PROTOCOL_C_API FLOWIE_PROTOCOL_API
#endif

#endif /* FLOWIE_PROTOCOL_EXPORT_H */
