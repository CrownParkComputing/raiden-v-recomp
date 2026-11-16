/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x86-64 guest calls into the real Tempest NGS2 runtime on an ARM64 host. */
#undef NDEBUG
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "fex_c_adapter.h"
#include "core/libraries/kernel/threads/exception.h"
#include <FEXCore/Core/X86Enums.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "runtime.h"
extern const TempestFexExport tempest_fex_runtime_ngs2[];
extern const size_t tempest_fex_runtime_ngs2_count;
extern const TempestFexExport tempest_fex_fex_abi_fixture[];
extern const size_t tempest_fex_fex_abi_fixture_count;
void *runtime_low_alloc(size_t size) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *p = mmap(nullptr, (size + page - 1) & ~(page - 1), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (reinterpret_cast<uintptr_t>(p) >= (UINT64_C(1) << 40)) {
        munmap(p, (size + page - 1) & ~(page - 1));
        return nullptr;
    }
    return p;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(table[i].name, name)) return reinterpret_cast<uintptr_t>(table[i].function);
    return 0;
}
}

namespace {
using Ranges = std::vector<Core::GuestExecutionRange>;
thread_local const Ranges *active_ranges;
struct Stack {
    size_t size;
    void *address;
    explicit Stack(size_t bytes) : size(bytes), address(mmap(nullptr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) { assert(address != MAP_FAILED); }
    ~Stack() { assert(munmap(address, size) == 0); }
};
bool validate(void *context, uintptr_t address, size_t size, bool writable) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    for (const auto &r : active_ranges ? *active_ranges : *static_cast<Ranges *>(context))
        if (address >= r.Begin && address - r.Begin <= r.Size &&
            size <= r.Size - (address - r.Begin) && (!writable || r.Writable)) return true;
    return false;
}
static volatile std::sig_atomic_t handled_shadow_guard_segv=0;
void fault(int signal, siginfo_t *info, void *context) {
    if (!Core::Fex::HandleGuestSignal(signal, info, context)) _exit(128 + signal);
    if (signal==SIGSEGV) handled_shadow_guard_segv=handled_shadow_guard_segv+1;
}
uintptr_t bind(Core::GuestCpu::HleCallRegistry &registry, Core::GuestCpu::HleVeneerAllocator &veneers,
               const char *name) {
    auto address = runtime_ngs2_resolve(name);
    std::shared_ptr<Core::GuestCpu::HleCallAdapter> adapter;
    for (size_t i = 0; i < tempest_fex_runtime_ngs2_count; ++i)
        if (address && reinterpret_cast<uintptr_t>(tempest_fex_runtime_ngs2[i].function) == address)
            adapter = registry.Register(std::make_shared<Tempest::CAdapter>(tempest_fex_runtime_ngs2[i]), name);
    for (size_t i = 0; i < tempest_fex_fex_abi_fixture_count; ++i)
        if (!std::strcmp(tempest_fex_fex_abi_fixture[i].name, name))
            adapter = registry.Register(std::make_shared<Tempest::CAdapter>(tempest_fex_fex_abi_fixture[i]), name);
    assert(adapter);
    auto result = veneers.Allocate(*adapter);
    assert(std::holds_alternative<u64>(result));
    return std::get<u64>(result);
}
class QueuedSignal final : public Core::GuestCpu::HleCallAdapter {
public:
    explicit QueuedSignal(uintptr_t handler_) : handler(handler_) {}
    Core::GuestCpu::HleCallResult Invoke(Core::GuestCpu::HleCallFrame &frame) const override {
        assert(Core::Fex::DeliverGuestOrbisSignal(30,nullptr,nullptr,handler));
        frame.gpr[FEXCore::X86State::REG_RAX]=0xc001beef;
        return true;
    }
private:
    uintptr_t handler;
};
}

int main() {
    struct sigaction action {};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGBUS, &action, nullptr) == 0);
    assert(sigaction(SIGSEGV, &action, nullptr) == 0);

    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *memory = static_cast<unsigned char *>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    auto *guest_code = static_cast<unsigned char *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(guest_code != MAP_FAILED);
    const unsigned char code[] = {0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3, /* mov rax,gs:[0]; ret */
                                   0x48,0x8b,0x07,0xc3}; /* mov rax,[rdi]; ret */
    std::memcpy(guest_code, code, sizeof(code));
    // Each CALL consumes a shadow entry, while ADD balances the architectural
    // stack without popping that entry. CALL skips two NOPs, so this cannot be
    // optimized as the CALL+0 get-RIP idiom. The count below crosses at least two
    // lower guard pages when starting one quarter into the 4 MiB shadow region.
    const unsigned char shadow_wrap_code[]={
        0x48,0x89,0xe2,                    // mov rdx,rsp (original guest stack)
        0x89,0xf9,                         // mov ecx,edi (iteration count)
        0xe8,0x02,0x00,0x00,0x00,         // loop: call target (nonzero displacement)
        0x90,0x90,                         // skipped NOPs
        0x48,0x83,0xc4,0x08,              // target: add rsp,8
        0xff,0xc9,                         // dec ecx
        0x75,0xf1,                         // jnz loop
        0xb8,0x2a,0x00,0x00,0x00,         // mov eax,42
        0xc3};                            // real RET after the wrapped calls
    std::memcpy(guest_code+256,shadow_wrap_code,sizeof(shadow_wrap_code));
    // An actual x86 signal handler records its two arguments and real interrupted
    // RSP/RIP/RDI, then deliberately clobbers a GPR, SIMD register and carry flag.
    const uintptr_t capture=reinterpret_cast<uintptr_t>(memory+160);
    std::vector<unsigned char> signal_code{0x48,0xb8}; // movabs rax,capture
    const auto append=[&](const auto &value) {
        const auto *bytes=reinterpret_cast<const unsigned char *>(&value);
        signal_code.insert(signal_code.end(),bytes,bytes+sizeof(value));
    };
    append(capture);
    signal_code.insert(signal_code.end(),{0x48,0x89,0x38}); // mov [rax],rdi
    const uint32_t offsets[]={
        offsetof(Libraries::Kernel::Ucontext,uc_mcontext)+offsetof(Libraries::Kernel::Mcontext,mc_rsp),
        offsetof(Libraries::Kernel::Ucontext,uc_mcontext)+offsetof(Libraries::Kernel::Mcontext,mc_rip),
        offsetof(Libraries::Kernel::Ucontext,uc_mcontext)+offsetof(Libraries::Kernel::Mcontext,mc_rdi)};
    static_assert(offsetof(Libraries::Kernel::Ucontext,uc_mcontext)+
                  offsetof(Libraries::Kernel::Mcontext,mc_rsp)==0xf8);
    for (unsigned i=0;i<3;++i) {
        signal_code.insert(signal_code.end(),{0x48,0x8b,0x8e}); // mov rcx,[rsi+offset]
        append(offsets[i]);
        signal_code.insert(signal_code.end(),{0x48,0x89,0x48,static_cast<unsigned char>(8*(i+1))});
    }
    signal_code.insert(signal_code.end(),{0x48,0xbb}); // movabs rbx,0xdeadbeef
    append(UINT64_C(0xdeadbeef));
    signal_code.insert(signal_code.end(),{0x66,0x0f,0xef,0xc0,0xf9,0xc3}); // pxor xmm0,xmm0; stc; ret
    std::memcpy(guest_code+64,signal_code.data(),signal_code.size());
    assert(mprotect(guest_code, page, PROT_READ | PROT_EXEC) == 0);
    Core::GuestCpu::HleCallRegistry registry;
    Core::GuestCpu::HleVeneerAllocator veneers;
    auto frame_info = bind(registry, veneers, "sceNgs2GetWaveformFrameInfo");
    auto create = bind(registry, veneers, "sceNgs2SystemCreateWithAllocator");
    auto destroy = bind(registry, veneers, "sceNgs2SystemDestroy");
    auto negative = bind(registry, veneers, "fixture_negative");
    auto store = bind(registry, veneers, "fixture_store");
    auto mixed = bind(registry, veneers, "fixture_mixed");
    auto signal_adapter=registry.Register(std::make_shared<QueuedSignal>(
        reinterpret_cast<uintptr_t>(guest_code+64)),"queued-signal");
    auto signal_veneer=veneers.Allocate(*signal_adapter);
    assert(std::holds_alternative<u64>(signal_veneer));
    Ranges ranges = veneers.GetExecutableRanges();
    ranges.push_back({reinterpret_cast<uintptr_t>(memory), page * 3, false, true});
    ranges.push_back({reinterpret_cast<uintptr_t>(guest_code), page, true, false});
    Core::GuestCpu::HleGuestBridge bridge(registry, validate, &ranges);
    auto created = Core::FexGuestCpuBackend::Create(bridge);
    assert(std::holds_alternative<std::unique_ptr<Core::FexGuestCpuBackend>>(created));
    auto backend = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(created));
    ranges.push_back(backend->ReturnRange());
    ranges.push_back(backend->CallbackReturnRange());
    const uintptr_t return_address = backend->ReturnAddress();
    const auto execute = [&](uintptr_t entry, std::initializer_list<uint64_t> arguments,
                             std::initializer_list<double> floating, uintptr_t gs_base = 0) {
        Core::GuestExecutionRequest request;
        request.Rip = entry;
        /* Each caller needs its own stack when two guest threads execute concurrently. */
        thread_local Stack thread_stack(page);
        request.Rsp = reinterpret_cast<uintptr_t>(thread_stack.address) + page - 24;
        request.GsBase = gs_base;
        std::memcpy(reinterpret_cast<void *>(request.Rsp), &return_address, sizeof(uint64_t));
        constexpr size_t registers[] = {FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI,
            FEXCore::X86State::REG_RDX, FEXCore::X86State::REG_RCX,
            FEXCore::X86State::REG_R8, FEXCore::X86State::REG_R9};
        size_t index = 0;
        for (uint64_t argument : arguments) {
            if (index < std::size(registers)) request.Gpr[registers[index]] = argument;
            else std::memcpy(reinterpret_cast<void *>(request.Rsp + 8 * (index - 5)), &argument, 8);
            ++index;
        }
        index = 0;
        for (double argument : floating) {
            assert(index < 8);
            std::memcpy(&request.Xmm[index++][0], &argument, sizeof(argument));
        }
        request.MappedRanges = ranges;
        request.MappedRanges.push_back({reinterpret_cast<uintptr_t>(thread_stack.address), page, false, true});
        active_ranges = &request.MappedRanges;
        auto result = backend->Run(request);
        active_ranges = nullptr;
        if (auto *failure = std::get_if<Core::GuestExecutionFailure>(&result)) {
            std::fprintf(stderr, "Tempest FEX check failed at entry 0x%lx, stage %d: %d\n",
                         entry, static_cast<int>(failure->Stage), failure->Error);
            std::abort();
        }
        auto &state = std::get<Core::GuestExecutionState>(result);
        assert(state.StopReason == Core::GuestStopReason::Halted);
        if (state.Rip != backend->ReturnAddress() && state.Rip != backend->ReturnAddress() + 1) {
            std::fprintf(stderr, "Unexpected guest stop RIP 0x%lx (return veneer 0x%lx)\n",
                         state.Rip, backend->ReturnAddress());
            std::abort();
        }
        return state;
    };
    const auto call = [&](uintptr_t entry, std::initializer_list<uint64_t> arguments) {
        return static_cast<int32_t>(execute(entry, arguments, {}).Gpr[FEXCore::X86State::REG_RAX]);
    };
    constexpr uint64_t shadow_calls=131104;
    handled_shadow_guard_segv=0;
    const auto wrapped=execute(reinterpret_cast<uintptr_t>(guest_code+256),{shadow_calls},{});
    assert(handled_shadow_guard_segv>=2); // prove real guard faults, not an optimized-away CALL
    assert(wrapped.Gpr[FEXCore::X86State::REG_RAX]==42);
    assert(wrapped.Gpr[FEXCore::X86State::REG_RDI]==shadow_calls);
    assert(wrapped.Gpr[FEXCore::X86State::REG_RCX]==0);
    assert(wrapped.Gpr[FEXCore::X86State::REG_RSP]==
           wrapped.Gpr[FEXCore::X86State::REG_RDX]+8);
    // All regular HLE, signed/FP/stack-argument, GS-TLS and queued-signal checks
    // below still execute after shadow-wrap recovery and a normal guest RET.
    uint32_t format[] = {0x40, 2, 48000, 0xfe740bf0, 0, 0};
    std::memcpy(memory, format, sizeof(format));
    const auto at = [&](size_t offset) { return reinterpret_cast<uintptr_t>(memory + offset); };
    assert(call(frame_info, {at(0), at(32), at(36), at(40), at(44)}) == 0);
    uint32_t geometry[4];
    std::memcpy(geometry, memory + 32, sizeof(geometry));
    assert(geometry[0] == 384 && geometry[1] == 1024 && geometry[2] == 4 && geometry[3] == 256);
    assert(call(create, {0, 0, at(64)}) == 0);
    uintptr_t system;
    std::memcpy(&system, memory + 64, sizeof(system));
    assert(system && system < (UINT64_C(1) << 40));
    assert(call(destroy, {system, 0}) == 0);
    assert(munmap(reinterpret_cast<void *>(system), page) == 0);
    assert(execute(negative, {}, {}).Gpr[FEXCore::X86State::REG_RAX] == UINT64_C(0xffffffdb));
    execute(store, {at(80), UINT64_C(0x123456789abcdef0)}, {});
    uint64_t stored;
    std::memcpy(&stored, memory + 80, sizeof(stored));
    assert(stored == UINT64_C(0x123456789abcdef0));
    auto mixed_result = execute(mixed, {1, 2, 3, 4, 5, 6, 7}, {1.5, 2.25});
    double value;
    std::memcpy(&value, &mixed_result.Xmm[0][0], sizeof(value));
    assert(value == 31.375);
    assert(!Core::Fex::DeliverGuestOrbisSignal(30,nullptr,nullptr,
        reinterpret_cast<uintptr_t>(guest_code+64))); // no active guest on this host thread
    const auto signal_result=execute(std::get<u64>(signal_veneer),{0x12345678},{7.25});
    uint64_t captured[4]; std::memcpy(captured,memory+160,sizeof(captured));
    assert(captured[0]==30 && captured[1] && captured[3]==0x12345678);
    assert(std::ranges::any_of(ranges,[&](const auto &range) {
        return range.Executable && captured[2]>=range.Begin && captured[2]-range.Begin<range.Size;
    }));
    assert(signal_result.Gpr[FEXCore::X86State::REG_RAX]==0xc001beef);
    assert(signal_result.Gpr[FEXCore::X86State::REG_RBX]==0);
    std::memcpy(&value,&signal_result.Xmm[0][0],sizeof(value));
    assert(value==7.25 && !(signal_result.Rflags&1));
    assert(!Core::Fex::PendingGuestOrbisSignal());
    const uint64_t tls[2] = {UINT64_C(0x1234567890abcdef), UINT64_C(0xfedcba0987654321)};
    std::memcpy(memory + 96, tls, sizeof(tls));
    std::thread first([&] {
        assert(execute(reinterpret_cast<uintptr_t>(guest_code), {}, {}, at(96)).Gpr[0] == tls[0]);
    });
    std::thread second([&] {
        assert(execute(reinterpret_cast<uintptr_t>(guest_code), {}, {}, at(104)).Gpr[0] == tls[1]);
    });
    first.join(); second.join();
    std::memcpy(memory + 113, &tls[0], sizeof(tls[0]));
    assert(execute(reinterpret_cast<uintptr_t>(guest_code + 10), {at(113)}, {}).Gpr[0] == tls[0]);
    {
        Stack checked_stack(page);
        Stack checked_data(page * 3);
        Core::GuestExecutionRequest request;
        request.Rip = reinterpret_cast<uintptr_t>(guest_code);
        request.Rsp = reinterpret_cast<uintptr_t>(checked_stack.address) + page - 24;
        request.MappedRanges = ranges;
        request.MappedRanges.push_back({reinterpret_cast<uintptr_t>(checked_stack.address), page, false, true});
        request.MappedRanges.push_back({reinterpret_cast<uintptr_t>(checked_data.address), page * 3, false, true});
        const auto expect_mapping = [&](int expected_error) {
            auto result = backend->CreateThread(request);
            if (!expected_error) {
                assert(std::holds_alternative<std::unique_ptr<Core::FexGuestCpuBackend::Thread>>(result));
                return;
            }
            const auto *failure = std::get_if<Core::GuestExecutionFailure>(&result);
            assert(failure && failure->Stage == Core::GuestExecutionStage::Mapping &&
                   failure->Error == expected_error);
        };
        expect_mapping(0);
        // Reuse the same declaration: later requests must observe revoked protection.
        assert(mprotect(guest_code, page, PROT_READ | PROT_WRITE) == 0);
        expect_mapping(EACCES);
        // FEX translates readable guest code; host execute permission is unnecessary.
        assert(mprotect(guest_code, page, PROT_READ) == 0);
        expect_mapping(0);
        auto *middle = static_cast<unsigned char *>(checked_data.address) + page;
        assert(mprotect(middle, page, PROT_READ) == 0);
        expect_mapping(EACCES);
        assert(mprotect(middle, page, PROT_READ | PROT_WRITE) == 0);
        expect_mapping(0);
        assert(munmap(middle, page) == 0);
        expect_mapping(EFAULT);
        // Stack's full-range munmap also releases the two pages around the hole.
    }
    backend.reset();
    assert(munmap(guest_code, page) == 0);
    assert(munmap(memory, page * 3) == 0);
    std::puts("Tempest FEX: x86 guest -> ARM64 NGS2 geometry and system lifecycle passed");
    std::puts("Tempest FEX: generated adapters preserve pointers, void, signed returns, FP and stack arguments");
    std::puts("Tempest FEX: concurrent x86 threads, GS TLS and unaligned loads passed");
    std::puts("FEX: queued x86 exception handler, authentic context and GPR/SIMD/flags restoration passed");
    std::puts("FEX: fresh host mappings reject changed permissions and gaps; readable NX guest code accepted");
    std::puts("FEX: repeated CALL shadow-stack guard wraps recover and return normally");
    return 0;
}
