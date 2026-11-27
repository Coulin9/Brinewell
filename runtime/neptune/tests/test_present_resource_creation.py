import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


source_dir = Path(__file__).resolve().parents[1]
source = (source_dir / "npt_overrides_d3d12_device.c").read_text()
override = re.search(
    r"static HRESULT NPT_STDMETHODCALLTYPE\n"
    r"dev12_CreateCommittedResource_override\([^)]*\)\n\{.*?^\}",
    source,
    re.S | re.M,
)
if not override:
    raise RuntimeError("CreateCommittedResource override definition not found")

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define NPT_STDMETHODCALLTYPE
#define NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF 0x40000000u
#define NPT_S_OK 0
#define NPT_E_FAIL (-1)
#define NPT_E_OUTOFMEMORY (-2)
#define NPT_E_INVALIDARG (-3)
#define NPT_FAILED(hr) ((hr) < 0)
#define NPT_SUCCEEDED(hr) ((hr) >= 0)

typedef int32_t HRESULT;
typedef uint32_t D3D12_HEAP_FLAGS, D3D12_RESOURCE_STATES, IID;
typedef struct { int Type; } D3D12_HEAP_PROPERTIES;
typedef struct { int Dimension; uint64_t Width; } D3D12_RESOURCE_DESC;
typedef struct { int Format; } D3D12_CLEAR_VALUE;

struct npt_device { bool multi_ring_enabled; };
struct npt_ring { int id; };
struct npt_com_base { uint64_t id; };
struct dev12_placed_args {
   const D3D12_RESOURCE_DESC *desc;
   D3D12_RESOURCE_STATES initial_state;
};

static const IID resource_iid = 7;
static const D3D12_HEAP_PROPERTIES heap = { .Type = 1 };
static const D3D12_RESOURCE_DESC resource = { .Dimension = 3, .Width = 64 };
static const D3D12_CLEAR_VALUE clear = { .Format = 28 };
static const D3D12_HEAP_FLAGS export_flags =
   1u | NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF;
static struct npt_device device;
static struct npt_ring ring = { .id = 2 };
static struct npt_com_base device_wrapper = { .id = 4 };
static int wrapped_resource;
static HRESULT host_result;
static uint64_t host_id;
static bool wrap_failed, replied;
static unsigned calls, wraps, releases, default_calls;

static struct npt_ring *npt_com_self_ring(void *self)
{
   assert(self == &device_wrapper);
   return &ring;
}

static uint64_t npt_com_self_id(void *self)
{
   assert(self == &device_wrapper);
   return device_wrapper.id;
}

static struct npt_device *npt_com_self_device(void *self)
{
   assert(self == &device_wrapper);
   return &device;
}

static HRESULT npt_call_ID3D12Device_CreateCommittedResource(
   struct npt_ring *target, uint64_t self, const D3D12_HEAP_PROPERTIES *props,
   D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
   D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear,
   const IID *iid, void **out)
{
   assert(target == &ring && self == device_wrapper.id);
   assert(props == &heap && flags == export_flags && desc == &resource);
   assert(initial_state == 5 && optimized_clear == &clear && iid == &resource_iid);
   assert(out && !*out && !replied);
   ++calls;
   *out = (void *)(uintptr_t)host_id;
   replied = true;
   return host_result;
}

static void *npt_com_get_or_wrap_or_release(struct npt_device *dev,
                                           const IID *iid, uint64_t id,
                                           struct npt_com_base *parent)
{
   assert(dev == &device && iid == &resource_iid && parent == &device_wrapper);
   assert(replied && host_result == NPT_S_OK && id && id == host_id);
   ++wraps;
   return wrap_failed ? NULL : &wrapped_resource;
}

static void npt_ring_send_com_release_immediate(struct npt_device *dev,
                                               uint64_t id)
{
   assert(dev == &device && replied && host_result != NPT_S_OK && id && id == host_id);
   ++releases;
}

static bool dev12_committed_shmem_candidate(const D3D12_HEAP_PROPERTIES *props,
                                           D3D12_HEAP_FLAGS flags, int dimension,
                                           uint64_t width, const IID *iid,
                                           void **out)
{
   assert(props == &heap && dimension == resource.Dimension && width == resource.Width);
   assert(iid == &resource_iid && (flags == 1 || flags == export_flags));
   (void)out;
   return false;
}

static HRESULT dev12_committed_shmem_create(
   void *self, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
   const struct dev12_placed_args *placed, uint64_t width,
   const D3D12_CLEAR_VALUE *optimized_clear, const IID *iid, void **out)
{
   (void)self; (void)props; (void)flags; (void)placed; (void)width;
   (void)optimized_clear; (void)iid; (void)out;
   assert(false);
   return NPT_E_FAIL;
}

static void npt_log(const char *format, ...)
{
   (void)format;
}

static HRESULT npt_id3d12device_default_CreateCommittedResource(
   void *self, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
   const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
   const D3D12_CLEAR_VALUE *optimized_clear, const IID *iid, void **out)
{
   assert(self == &device_wrapper && props == &heap && desc == &resource);
   assert((flags == 1 || flags == export_flags) && initial_state == 5);
   assert(optimized_clear == &clear && iid == &resource_iid);
   if (out)
      *out = &wrapped_resource;
   ++default_calls;
   return NPT_S_OK;
}
"""

suffix = r"""
static void test_export(HRESULT status, uint64_t id, bool fail_wrap,
                        HRESULT expected, unsigned expected_wraps,
                        unsigned expected_releases)
{
   host_result = status;
   host_id = id;
   wrap_failed = fail_wrap;
   replied = false;
   calls = wraps = releases = default_calls = 0;
   void *out = &device_wrapper;
   HRESULT result = dev12_CreateCommittedResource_override(
      &device_wrapper, &heap, export_flags, &resource, 5, &clear, &resource_iid, &out);
   assert(replied && calls == 1 && default_calls == 0);
   assert(result == expected);
   assert(out == (expected == NPT_S_OK ? &wrapped_resource : NULL));
   assert(wraps == expected_wraps && releases == expected_releases);
}

int main(void)
{
   test_export(NPT_S_OK, 31, false, NPT_S_OK, 1, 0);
   device.multi_ring_enabled = true;
   test_export(NPT_S_OK, 32, false, NPT_S_OK, 1, 0);
   test_export(NPT_E_INVALIDARG, 33, false, NPT_E_INVALIDARG, 0, 1);
   test_export(NPT_E_FAIL, 0, false, NPT_E_FAIL, 0, 0);
   test_export(NPT_S_OK, 0, false, NPT_E_FAIL, 0, 0);
   test_export(NPT_S_OK, 34, true, NPT_E_OUTOFMEMORY, 1, 0);

   void *out = NULL;
   assert(dev12_CreateCommittedResource_override(
      &device_wrapper, &heap, 1, &resource, 5, &clear, &resource_iid, &out) == NPT_S_OK);
   assert(out == &wrapped_resource);
   assert(dev12_CreateCommittedResource_override(
      &device_wrapper, &heap, export_flags, &resource, 5, &clear, &resource_iid, NULL) == NPT_S_OK);
   assert(default_calls == 2);
   puts("PASS synchronous present resource creation, failure handling, and fallback");
   return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="npt-present-resource-") as directory:
    directory = Path(directory)
    test_source = directory / "test.c"
    executable = directory / "test"
    test_source.write_text(prefix + override[0] + suffix)
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    subprocess.run(command + [str(test_source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
