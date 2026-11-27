/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side NGX parameter container for nvngx.dll.  The object callers
 * receive is a struct whose first member is a vtable pointer laid out in
 * the frozen caller ABI slot order (see nvngx.cpp's ngx_param_vtbl);
 * nothing here uses C++ virtuals so the slot order is explicit.  Values
 * are stored type-tagged; the Get paths mirror the Mullion interop set:
 * pointer/resource both ways, int<->uint, int/uint<->float, double<->
 * float, and ull<->void-ptr/int/uint.  Unknown keys are kept for local
 * but are never serialized.
 */

#ifndef NGX_PARAMETER_HPP
#define NGX_PARAMETER_HPP

#include <climits>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

struct NgxParamValue {
   enum Kind { KIND_INT, KIND_NUM, KIND_PTR } kind;
   uint64_t i;
   double d;
   void *p;
};

struct NgxParameters {
   const void *const *vt;
   std::mutex mtx;
   std::unordered_map<std::string, NgxParamValue> values;
};

#define NGX_RESULT_OK                1u
#define NGX_RESULT_FAIL              0xBAD00000u
#define NGX_RESULT_NOT_SUPPORTED     0xBAD00001u
#define NGX_RESULT_INVALID_PARAMETER 0xBAD00005u

static inline bool
ngx_param_failed(uint32_t r)
{
   return (r & NGX_RESULT_FAIL) == NGX_RESULT_FAIL;
}

template <typename T>
static void
ngx_param_set(NgxParameters *self, const char *name, T value);

template <>
inline void
ngx_param_set<unsigned long long>(NgxParameters *self, const char *name,
                                  unsigned long long value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_INT, value, 0.0, nullptr};
}

template <>
inline void
ngx_param_set<int>(NgxParameters *self, const char *name, int value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_INT,
                         (uint64_t)(int64_t)value, 0.0, nullptr};
}

template <>
inline void
ngx_param_set<unsigned int>(NgxParameters *self, const char *name,
                            unsigned int value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_INT, value, 0.0, nullptr};
}

template <>
inline void
ngx_param_set<float>(NgxParameters *self, const char *name, float value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_NUM, 0, value, nullptr};
}

template <>
inline void
ngx_param_set<double>(NgxParameters *self, const char *name, double value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_NUM, 0, value, nullptr};
}

template <>
inline void
ngx_param_set<void *>(NgxParameters *self, const char *name, void *value)
{
   std::lock_guard<std::mutex> lock(self->mtx);
   self->values[name] = {NgxParamValue::KIND_PTR, 0, 0.0, value};
}

/* Lookup without locking: callers snapshot under the session mutex, which
 * serializes Set/Get against the wire paths. */
static inline bool
ngx_param_find(const NgxParameters *self, const char *name,
               NgxParamValue *out)
{
   auto it = self->values.find(name);
   if (it == self->values.end())
      return false;
   *out = it->second;
   return true;
}

enum NgxExactUint32Result { NGX_UINT_ABSENT, NGX_UINT_OK, NGX_UINT_INVALID };

static inline NgxExactUint32Result
ngx_param_try_get_exact_uint32(const NgxParameters *self, const char *name,
                               uint32_t *out)
{
   NgxParamValue v;
   if (!ngx_param_find(self, name, &v))
      return NGX_UINT_ABSENT;
   if (v.kind == NgxParamValue::KIND_INT && v.i <= UINT32_MAX) {
      *out = (uint32_t)v.i;
      return NGX_UINT_OK;
   }
   if (v.kind == NgxParamValue::KIND_NUM && std::isfinite(v.d) &&
       std::trunc(v.d) == v.d && v.d >= 0.0 && v.d <= 4294967295.0) {
      *out = (uint32_t)v.d;
      return NGX_UINT_OK;
   }
   return NGX_UINT_INVALID;
}

template <typename T>
static bool
ngx_param_convert(const NgxParamValue &v, T *out);

/* Resource pointer reads behave like void* reads. */
template <typename R>
static inline bool
ngx_param_convert(const NgxParamValue &v, R **out)
{
   return ngx_param_convert<void *>(v, (void **)out);
}

template <>
inline bool
ngx_param_convert<void *>(const NgxParamValue &v, void **out)
{
   if (v.kind == NgxParamValue::KIND_PTR) {
      *out = v.p;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (void *)(uintptr_t)v.i;
      return true;
   }
   return false;
}

template <>
inline bool
ngx_param_convert<unsigned long long>(const NgxParamValue &v,
                                      unsigned long long *out)
{
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (unsigned long long)v.i;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_PTR) {
      *out = (unsigned long long)(uintptr_t)v.p;
      return true;
   }
   return false;
}

template <>
inline bool
ngx_param_convert<int>(const NgxParamValue &v, int *out)
{
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (int)v.i;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_NUM && std::isfinite(v.d) &&
       v.d >= (double)INT_MIN && v.d <= (double)INT_MAX) {
      *out = (int)v.d;
      return true;
   }
   return false;
}

template <>
inline bool
ngx_param_convert<unsigned int>(const NgxParamValue &v, unsigned int *out)
{
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (unsigned int)v.i;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_NUM && std::isfinite(v.d) &&
       v.d >= 0.0 && v.d <= (double)UINT_MAX) {
      *out = (unsigned int)v.d;
      return true;
   }
   return false;
}

template <>
inline bool
ngx_param_convert<float>(const NgxParamValue &v, float *out)
{
   if (v.kind == NgxParamValue::KIND_NUM) {
      *out = (float)v.d;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (float)(int64_t)v.i;
      return true;
   }
   return false;
}

template <>
inline bool
ngx_param_convert<double>(const NgxParamValue &v, double *out)
{
   if (v.kind == NgxParamValue::KIND_NUM) {
      *out = v.d;
      return true;
   }
   if (v.kind == NgxParamValue::KIND_INT) {
      *out = (double)(int64_t)v.i;
      return true;
   }
   return false;
}

template <typename T>
static uint32_t
ngx_param_get(const NgxParameters *self, const char *name, T *out)
{
   if (!out)
      return NGX_RESULT_INVALID_PARAMETER;
   std::lock_guard<std::mutex> lock(
      const_cast<NgxParameters *>(self)->mtx);
   NgxParamValue v;
   if (!ngx_param_find(self, name, &v) || !ngx_param_convert(v, out))
      return NGX_RESULT_FAIL;
   return NGX_RESULT_OK;
}

#endif /* NGX_PARAMETER_HPP */
