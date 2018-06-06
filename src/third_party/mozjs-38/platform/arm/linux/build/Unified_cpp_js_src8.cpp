#define MOZ_UNIFIED_BUILD
#include "json.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "json.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "json.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsopcode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsopcode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsopcode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsprf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsprf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsprf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jspropertytree.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jspropertytree.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jspropertytree.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsreflect.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsreflect.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsreflect.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsscript.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsscript.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsscript.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsstr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsstr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsstr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jswatchpoint.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jswatchpoint.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jswatchpoint.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsweakmap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsweakmap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsweakmap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "perf/jsperf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "perf/jsperf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "perf/jsperf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "prmjtime.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "prmjtime.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "prmjtime.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/BaseProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/BaseProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/BaseProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/CrossCompartmentWrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/CrossCompartmentWrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/CrossCompartmentWrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/DeadObjectProxy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/DeadObjectProxy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/DeadObjectProxy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/DirectProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/DirectProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/DirectProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/Proxy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/Proxy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/Proxy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif