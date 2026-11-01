/* Off-screen frames on Windows: a view with no window under --headless, each
 * frame's pixels read at once and through its file mapping. The page is pure
 * red on the left and pure blue on the right.
 *
 * Exit 0 when every check passes. */

#define COBJMACROS
#include <windows.h>

#include <d3d11_1.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/offscreen.h>
#include <zebkit/zebkit.h>

static const char page[] =
    "data:text/html,<!doctype html><title>offscreen</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static int failures, frames;
static zebkit_view *view;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        ++failures;
}

static uint32_t at(const uint8_t *pixels, size_t stride, int x, int y) {
    uint32_t value;
    memcpy(&value, pixels + (size_t)y * stride + (size_t)x * 4, 4);
    return value & 0xffffff;
}

/* A D3D11 frame: the texture opened by its handle, copied where the CPU reads. */
static void check_texture(const zebkit_frame *f) {
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
    check(SUCCEEDED(hr), "a D3D11 device of the application's");
    if (FAILED(hr))
        return;
    ID3D11Device1 *device1 = NULL;
    ID3D11Texture2D *shared = NULL, *staging = NULL;
    ID3D11Device_QueryInterface(device, &IID_ID3D11Device1, (void **)&device1);
    hr = ID3D11Device1_OpenSharedResource1(device1, (HANDLE)f->handle, &IID_ID3D11Texture2D, (void **)&shared);
    check(SUCCEEDED(hr), "OpenSharedResource1() of the frame's handle");
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(shared, &desc);
        printf("texture %ux%u format %d\n", desc.Width, desc.Height, (int)desc.Format);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
        IDXGIKeyedMutex *mutex = NULL;
        ID3D11Texture2D_QueryInterface(shared, &IID_IDXGIKeyedMutex, (void **)&mutex);
        if (mutex)
            IDXGIKeyedMutex_AcquireSync(mutex, 0, 2000);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)shared);
        if (mutex) {
            IDXGIKeyedMutex_ReleaseSync(mutex, 0);
            IDXGIKeyedMutex_Release(mutex);
        }
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) {
            check(at(map.pData, map.RowPitch, f->width / 4, f->height / 2) == 0xff0000, "the texture: red on the left");
            check(at(map.pData, map.RowPitch, f->width * 3 / 4, f->height / 2) == 0x0000ff, "the texture: blue on the right");
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
        ID3D11Texture2D_Release(shared);
    }
    ID3D11Device1_Release(device1);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
}

static void finish(void *data) {
    (void)data;
    printf("%d frames, %d failures\n", frames, failures);
    fflush(stdout);
    zebkit_view_close(view);
    zebkit_quit();
}

static void frame(zebkit_view *v, const zebkit_frame *f, void *data) {
    (void)v;
    (void)data;
    ++frames;
    printf("frame %llu %dx%d scale %.1f kind %d format %d stride %zu handle %p fd %d damage %zu\n",
           (unsigned long long)f->sequence, f->width, f->height, f->scale, (int)f->kind, (int)f->format, f->stride,
           f->handle, f->fd, f->damage_count);
    fflush(stdout);
    if (f->kind == ZEBKIT_FRAME_D3D11 && frames < 1000) {
        check_texture(f);
        frames = 1000;
        zebkit_post_task(finish, NULL);
    } else if (f->pixels && at(f->pixels, f->stride, f->width / 4, f->height / 2) == 0xff0000 && failures == 0 && frames < 1000) {
        check(1, "the frame's own pixels: red on the left");
        check(at(f->pixels, f->stride, f->width * 3 / 4, f->height / 2) == 0x0000ff, "the frame's own pixels: blue on the right");
        check(f->handle != NULL, "a file mapping's handle");
        check(f->fd == -1, "no file descriptor");
        if (f->handle) {
            const uint8_t *mapped = MapViewOfFile((HANDLE)f->handle, FILE_MAP_READ, 0, 0, f->size);
            check(mapped != NULL, "MapViewOfFile() of the handle");
            if (mapped) {
                check(at(mapped + f->offset, f->stride, f->width / 4, f->height / 2) == 0xff0000, "through the mapping: red");
                check(at(mapped + f->offset, f->stride, f->width * 3 / 4, f->height / 2) == 0x0000ff, "through the mapping: blue");
                UnmapViewOfFile(mapped);
            }
        }
        frames = 1000;
        zebkit_post_task(finish, NULL);
    }
    zebkit_frame_release(f);
}

static void timeout(void *data) {
    (void)data;
    check(0, "a frame with the page's picture in time");
    finish(NULL);
}

static void ready(void *data) {
    (void)data;
    check(zebkit_is_headless(), "headless");
    view = zebkit_view_new_offscreen(page, 800, 600, 1);
    check(view != NULL, "zebkit_view_new_offscreen()");
    if (!view) {
        zebkit_quit();
        return;
    }
    if (getenv("WIN_OFFSCREEN_GPU"))
        zebkit_view_set_frame_buffers(view, ZEBKIT_BUFFERS_GPU);
    zebkit_view_on_frame(view, frame, NULL);
    zebkit_post_delayed_task(timeout, NULL, 40000);
}

/* Where a crash is, to look up in the linker's map. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0x80000003 || code == 0xC0000005 || code == 0xC0000409) {
        HMODULE module = NULL;
        void *address = info->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module);
        printf("exception 0x%lx at RVA 0x%llx of module %p; stack:", code,
               (unsigned long long)((char *)address - (char *)module), (void *)module);
        void *frames[32];
        const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, NULL);
        for (USHORT i = 0; i < count; ++i)
            printf(" %llx", (unsigned long long)((char *)frames[i] - (char *)module));
        printf("\n");
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv) {
    if (getenv("WIN_OFFSCREEN_TRACE"))
        AddVectoredExceptionHandler(1, on_exception);
    const int code = zebkit_main(argc, argv, ready, NULL);
    printf("zebkit_main returned %d, %d failures\n", code, failures);
    return code ? code : (failures != 0);
}
