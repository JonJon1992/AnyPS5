// MinGW GCC has no native TLS: every thread_local access calls __emutls_get_address, and libgcc's
// version finds the thread's table through winpthreads' pthread_getspecific, which takes a spin
// lock per call. In ASTRO BOT's menu that was 8.5% of the graphics queue worker's time
// (Recorder::SyncThrough, collectWrites, the register lookups). This definition takes the calls
// made by this module (the linker prefers it to the libgcc_s import) and keeps the table pointer
// in a Windows TLS slot, read straight from the TEB: no lock, no call into another DLL, and the
// thread's last error is left alone. The control objects of other modules (libstdc++'s call_once
// variables, reached through this module's inline code) keep going to libgcc, which owns their
// offsets. APS5_LIBGCC_EMUTLS=1 sends every call to libgcc.
#if defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// libgcc's struct __emutls_object (word and pointer are 64-bit on x86_64).
struct EmutlsObject {
    std::uintptr_t size;
    std::uintptr_t align;
    std::uintptr_t offset;
    void* templ;
};

struct EmutlsArray {
    std::uintptr_t size;
    void* data[1];
};

// A thread's table, freed once the thread has exited: its handle is signaled only after every
// exit callback (the thread_local destructors among them) has run, so nothing frees a variable a
// destructor still uses.
struct ThreadTable {
    HANDLE thread;
    DWORD id;
    EmutlsArray* array;
    ThreadTable* next;
};

using GetAddress = void* (*)(EmutlsObject*);

std::atomic<int> state{0};  // 0: not set up, 1: own, 2: libgcc for every object
std::uintptr_t imageStart = 0;
std::uintptr_t imageEnd = 0;
DWORD slot = TLS_OUT_OF_INDEXES;
GetAddress libgcc = nullptr;
SRWLOCK lock = SRWLOCK_INIT;
std::uintptr_t offsets = 0;
ThreadTable* tables = nullptr;

GetAddress resolveLibgcc() {
    HMODULE module = GetModuleHandleW(L"libgcc_s_seh-1.dll");
    if (module == nullptr) module = LoadLibraryW(L"libgcc_s_seh-1.dll");
    const auto address = module != nullptr ? reinterpret_cast<GetAddress>(reinterpret_cast<void*>(GetProcAddress(module, "__emutls_get_address"))) : nullptr;
    if (address == nullptr) std::abort();
    return address;
}

int setUp() {
    AcquireSRWLockExclusive(&lock);
    int current = state.load(std::memory_order_relaxed);
    if (current == 0) {
        const auto error = GetLastError();
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&offsets), &module)) std::abort();
        imageStart = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageStart);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageStart + dos->e_lfanew);
        imageEnd = imageStart + nt->OptionalHeader.SizeOfImage;
        libgcc = resolveLibgcc();
        slot = std::getenv("APS5_LIBGCC_EMUTLS") == nullptr ? TlsAlloc() : TLS_OUT_OF_INDEXES;
        current = slot != TLS_OUT_OF_INDEXES ? 1 : 2;
        state.store(current, std::memory_order_release);
        SetLastError(error);
    }
    ReleaseSRWLockExclusive(&lock);
    return current;
}

// TlsGetValue without its SetLastError(0): TEB TlsSlots (0x1480) and TlsExpansionSlots (0x1780).
void* slotValue() {
    if (slot < 64) return reinterpret_cast<void*>(__readgsqword(0x1480 + slot * 8));
    auto** expansion = reinterpret_cast<void**>(__readgsqword(0x1780));
    return expansion != nullptr ? expansion[slot - 64] : nullptr;
}

std::uintptr_t assignOffset(EmutlsObject* object) {
    AcquireSRWLockExclusive(&lock);
    auto offset = __atomic_load_n(&object->offset, __ATOMIC_RELAXED);
    if (offset == 0) {
        offset = ++offsets;
        __atomic_store_n(&object->offset, offset, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&lock);
    return offset;
}

void freeTable(EmutlsArray* array) {
    for (std::uintptr_t index = 0; index < array->size; ++index) _aligned_free(array->data[index]);
    std::free(array);
}

// Under the lock: frees the tables of threads that have exited.
void reapExited() {
    for (ThreadTable** link = &tables; *link != nullptr;) {
        auto* table = *link;
        if (WaitForSingleObject(table->thread, 0) == WAIT_OBJECT_0) {
            *link = table->next;
            freeTable(table->array);
            CloseHandle(table->thread);
            std::free(table);
        } else {
            link = &table->next;
        }
    }
}

EmutlsArray* grow(EmutlsArray* array, std::uintptr_t offset) {
    const auto error = GetLastError();
    const std::uintptr_t size = offset + 32;
    auto* grown = static_cast<EmutlsArray*>(std::calloc(1, sizeof(EmutlsArray) + (size - 1) * sizeof(void*)));
    if (grown == nullptr) std::abort();
    grown->size = size;
    if (array != nullptr) std::memcpy(grown->data, array->data, array->size * sizeof(void*));
    AcquireSRWLockExclusive(&lock);
    if (array == nullptr) {
        reapExited();
        auto* table = static_cast<ThreadTable*>(std::malloc(sizeof(ThreadTable)));
        if (table == nullptr || !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &table->thread, SYNCHRONIZE, FALSE, 0)) std::abort();
        table->id = GetCurrentThreadId();
        table->array = grown;
        table->next = tables;
        tables = table;
    } else {
        const auto id = GetCurrentThreadId();
        for (auto* table = tables; table != nullptr; table = table->next) {
            if (table->id == id && table->array == array) {
                table->array = grown;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&lock);
    std::free(array);
    TlsSetValue(slot, grown);
    SetLastError(error);
    return grown;
}

void* allocate(const EmutlsObject* object) {
    const auto align = object->align > sizeof(void*) ? object->align : sizeof(void*);
    void* value = _aligned_malloc(object->size, align);
    if (value == nullptr) std::abort();
    if (object->templ != nullptr) std::memcpy(value, object->templ, object->size);
    else std::memset(value, 0, object->size);
    return value;
}

}

extern "C" void* __emutls_get_address(EmutlsObject* object) {
    int current = state.load(std::memory_order_acquire);
    if (current == 0) [[unlikely]] current = setUp();
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (current != 1 || address < imageStart || address >= imageEnd) [[unlikely]] return libgcc(object);
    auto offset = __atomic_load_n(&object->offset, __ATOMIC_ACQUIRE);
    if (offset == 0) [[unlikely]] offset = assignOffset(object);
    auto* array = static_cast<EmutlsArray*>(slotValue());
    if (array == nullptr || offset > array->size) [[unlikely]] array = grow(array, offset);
    void*& value = array->data[offset - 1];
    if (value == nullptr) [[unlikely]] {
        const auto error = GetLastError();
        value = allocate(object);
        SetLastError(error);
    }
    return value;
}

#endif
