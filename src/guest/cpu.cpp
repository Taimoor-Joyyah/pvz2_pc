#include <acp/guest.hpp>

#include <atomic>
#include <chrono>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace acp::guest {
Address TrapTable::add(std::string name, Handler handler) {
    require(traps_.size() < 65533, "Guest trap table exhausted");
    auto address = memory_.allocate(4096, Read | Write, 4096);
    auto id = static_cast<std::uint32_t>(traps_.size() + 1);
    memory_.store(address, 0xd4000001u | (id << 5));
    memory_.store(address + 4, 0xd65f03c0u);
    memory_.protect(address, 4096, Read | Execute);
    traps_.push_back({std::move(name), std::move(handler), address});
    return address;
}
const Trap &TrapTable::at(std::uint32_t id) const {
    require(id > 0 && id <= traps_.size(), std::format("Unknown guest trap {}", id));
    return traps_[id - 1];
}
std::vector<std::string> TrapTable::names() const {
    std::vector<std::string> out;
    for (const auto &t : traps_)
        out.push_back(t.name);
    std::ranges::sort(out);
    return out;
}

struct Cpu::Impl final : Dynarmic::A64::UserCallbacks {
    Cpu &owner;
    Memory &memory;
    TrapTable &traps;
    std::shared_ptr<Dynarmic::ExclusiveMonitor> monitor;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    Address stack{}, return_trap{}, thread_pointer{};
    std::uint64_t ticks{};
    std::uint32_t pending{};
    bool svc_pending{};
    std::string fault;
    std::atomic_bool stopped{};
    std::uint64_t instructions{}, calls{};
    std::uint64_t largest_call_instructions{}, largest_call_function{}, largest_call_elapsed_ms{};
    std::array<std::uint64_t, 65536> imports{};
    explicit Impl(Cpu &owner_, Memory &mem, TrapTable &table, size_t processor)
        : owner(owner_), memory(mem), traps(table) {
        static std::mutex lock;
        static std::map<Memory *, std::weak_ptr<Dynarmic::ExclusiveMonitor>> monitors;
        {
            std::lock_guard guard(lock);
            monitor = monitors[&memory].lock();
            if (!monitor) {
                monitor = std::make_shared<Dynarmic::ExclusiveMonitor>(256);
                monitors[&memory] = monitor;
            }
        }
        require(processor < 256, "Maximum guest thread count exceeded");
        Dynarmic::A64::UserConfig config{};
        config.callbacks = this;
        config.global_monitor = monitor.get();
        config.processor_id = processor;
        config.tpidr_el0 = &thread_pointer;
        config.tpidrro_el0 = &thread_pointer;
        config.page_table = memory.page_table();
        config.page_table_address_space_bits = 32;
        config.silently_mirror_page_table = false;
        config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = 32 << 20;
        config.cntfrq_el0 = 1'000'000'000;
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
        stack = memory.allocate(8 << 20, Read | Write, 4096);
        jit->SetSP(stack + (8 << 20) - 16);
        return_trap = memory.allocate(4096, Read | Write, 4096);
        memory.store(return_trap, 0xd41fffe1u);
        memory.protect(return_trap, 4096, Read | Execute);
        thread_pointer = memory.allocate(4096, Read | Write, 4096);
        memory.store(thread_pointer + 40, std::uint64_t{0xd913a6575e3100});
    }
    void fail(std::string message) {
        if (fault.empty())
            fault = std::move(message);
        jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }
    template <class T> T read(Address a) {
        try {
            return memory.load<T>(a);
        } catch (const std::exception &e) {
            fail(e.what());
            return {};
        }
    }
    template <class T> void write(Address a, T value) {
        try {
            memory.store(a, value);
        } catch (const std::exception &e) {
            fail(e.what());
        }
    }
    template <class T> bool exclusive(Address a, T desired, T expected) {
        try {
            return memory.compare_exchange(a, std::as_bytes(std::span(&expected, 1)),
                                           std::as_bytes(std::span(&desired, 1)));
        } catch (const std::exception &e) {
            fail(e.what());
            return false;
        }
    }
    std::optional<std::uint32_t> MemoryReadCode(Address a) override {
        if (!memory.valid(a, 4, Execute)) {
            fail(std::format("Guest execute fault at {:#x}", a));
            return {};
        }
        return read<std::uint32_t>(a);
    }
    std::uint8_t MemoryRead8(Address a) override {
        return read<std::uint8_t>(a);
    }
    std::uint16_t MemoryRead16(Address a) override {
        return read<std::uint16_t>(a);
    }
    std::uint32_t MemoryRead32(Address a) override {
        return read<std::uint32_t>(a);
    }
    std::uint64_t MemoryRead64(Address a) override {
        return read<std::uint64_t>(a);
    }
    Dynarmic::A64::Vector MemoryRead128(Address a) override {
        return read<Dynarmic::A64::Vector>(a);
    }
    void MemoryWrite8(Address a, std::uint8_t v) override {
        write(a, v);
    }
    void MemoryWrite16(Address a, std::uint16_t v) override {
        write(a, v);
    }
    void MemoryWrite32(Address a, std::uint32_t v) override {
        write(a, v);
    }
    void MemoryWrite64(Address a, std::uint64_t v) override {
        write(a, v);
    }
    void MemoryWrite128(Address a, Dynarmic::A64::Vector v) override {
        write(a, v);
    }
    bool MemoryWriteExclusive8(Address a, std::uint8_t v, std::uint8_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive16(Address a, std::uint16_t v, std::uint16_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive32(Address a, std::uint32_t v, std::uint32_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive64(Address a, std::uint64_t v, std::uint64_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive128(Address a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override {
        return exclusive(a, v, e);
    }
    void InterpreterFallback(Address pc, size_t) override {
        fail(std::format("Unsupported ARM64 instruction at {:#x}: {:#010x}", pc, read<std::uint32_t>(pc)));
    }
    void CallSVC(std::uint32_t id) override {
        pending = id;
        svc_pending = true;
        jit->HaltExecution();
    }
    void ExceptionRaised(Address pc, Dynarmic::A64::Exception exception) override {
        using E = Dynarmic::A64::Exception;
        if (exception == E::WaitForEvent || exception == E::WaitForInterrupt || exception == E::Yield ||
            exception == E::SendEvent || exception == E::SendEventLocal)
            return;
        fail(std::format("ARM64 exception {} at {:#x}", static_cast<int>(exception), pc));
    }
    void AddTicks(std::uint64_t passed) override {
        ticks = passed >= ticks ? 0 : ticks - passed;
    }
    std::uint64_t GetTicksRemaining() override {
        return ticks;
    }
    std::uint64_t GetCNTPCT() override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};
Cpu::Cpu(Memory &memory, TrapTable &traps, size_t processor)
    : memory_(memory), impl_(std::make_unique<Impl>(*this, memory, traps, processor)) {}
Cpu::~Cpu() = default;
std::uint64_t Cpu::call(Address function, std::span<const std::uint64_t> args, std::uint64_t budget,
                        std::chrono::milliseconds timeout) {
    require(memory_.valid(function, 4, Execute), std::format("Invalid guest call target {:#x}", function));
    auto &p = *impl_;
    ++p.calls;
    auto &jit = *p.jit;
    const auto regs = jit.GetRegisters();
    const auto vecs = jit.GetVectors();
    const auto saved_sp = jit.GetSP(), saved_pc = jit.GetPC(), saved_tls = p.thread_pointer,
               saved_ticks = p.ticks;
    const auto saved_pstate = jit.GetPstate(), saved_fpcr = jit.GetFpcr(), saved_fpsr = jit.GetFpsr();
    const auto saved_import = current_import;
    auto restore = [&] {
        jit.SetRegisters(regs);
        jit.SetVectors(vecs);
        jit.SetSP(saved_sp);
        jit.SetPC(saved_pc);
        jit.SetPstate(saved_pstate);
        jit.SetFpcr(saved_fpcr);
        jit.SetFpsr(saved_fpsr);
        p.thread_pointer = saved_tls;
        p.ticks = saved_ticks;
        current_import = saved_import;
        jit.ClearHalt(static_cast<Dynarmic::HaltReason>(UINT32_MAX));
    };
    const auto started = std::chrono::steady_clock::now();
    auto remaining = budget;
    try {
        require(timeout.count() > 0, "Guest call timeout must be positive");
        auto extra = args.size() > 8 ? (args.size() - 8) * 8 : 0;
        auto frame = align_up(extra + 64, 16);
        require(saved_sp >= p.stack + frame, "Guest stack exhausted");
        jit.SetSP(saved_sp - frame);
        for (size_t i = 0; i < args.size(); ++i) {
            if (i < 8)
                jit.SetRegister(i, args[i]);
            else
                memory_.store(jit.GetSP() + (i - 8) * 8, args[i]);
        }
        jit.SetRegister(30, p.return_trap);
        jit.SetPC(function);
        p.ticks = budget;
        p.fault.clear();
        // Instruction quotas and wall-clock deadlines are independent. Long-lived
        // pthread entry points explicitly opt out of both; frame calls retain a deadline.
        const auto deadline = timeout == std::chrono::milliseconds::max()
                                  ? std::chrono::steady_clock::time_point::max()
                                  : started + timeout;
        for (;;) {
            require(!p.stopped.load(), "Guest execution cancelled");
            if (checkpoint)
                checkpoint();
            require(remaining > 0, "Guest instruction budget exceeded");
            auto slice = std::min<std::uint64_t>(remaining, 1'000'000);
            p.ticks = slice;
            p.svc_pending = false;
            jit.ClearHalt(static_cast<Dynarmic::HaltReason>(UINT32_MAX));
            jit.Run();
            p.instructions += slice - p.ticks;
            remaining -= slice - p.ticks;
            if (!p.fault.empty())
                throw Error(p.fault);
            require(std::chrono::steady_clock::now() < deadline, "Guest call timeout");
            if (!p.svc_pending)
                continue;
            if (p.pending == 0xffff) {
                auto result = jit.GetRegister(0);
                if (budget - remaining > p.largest_call_instructions) {
                    p.largest_call_instructions = budget - remaining;
                    p.largest_call_function = function;
                    p.largest_call_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - started).count();
                }
                restore();
                return result;
            }
            if (!p.pending) {
                ++p.imports[0];
                require(bool(syscall), "Unhandled Android syscall");
                syscall(*this);
            } else {
                const auto &trap = p.traps.at(p.pending);
                ++p.imports[p.pending];
                current_import = trap.name;
                trap.handler(*this);
            }
        }
    } catch (const std::exception &error) {
        if (last_fault.is_null()) {
            last_fault = {{"pc", jit.GetPC()},        {"lr", jit.GetRegister(30)},
                          {"sp", jit.GetSP()},        {"tls", p.thread_pointer},
                          {"import", current_import}, {"registers", jit.GetRegisters()},
                          {"error", error.what()},
                          {"function", function}, {"instruction_budget", budget},
                          {"instructions_executed", budget - remaining},
                          {"timeout_ms", timeout.count()},
                          {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started).count()}};
            try {
                Json frames = Json::array();
                auto frame = jit.GetRegister(29);
                for (size_t depth = 0; depth < 128 && memory_.valid(frame, 16); ++depth) {
                    auto previous = memory_.load<Address>(frame), caller = memory_.load<Address>(frame + 8);
                    frames.push_back({{"frame", frame}, {"return_address", caller}});
                    if (previous <= frame || previous - frame > (8 << 20))
                        break;
                    frame = previous;
                }
                last_fault["stack"] = std::move(frames);
                Json objects = Json::array();
                for (size_t index : {0u, 19u, 20u}) {
                    auto address = jit.GetRegister(index);
                    if (!memory_.valid(address, 128))
                        continue;
                    Json object{{"register", index}, {"address", address}, {"words", Json::array()}};
                    for (size_t offset = 0; offset < 128; offset += 8)
                        object["words"].push_back(memory_.load<Address>(address + offset));
                    auto table = memory_.load<Address>(address);
                    if (memory_.valid(table, 80)) {
                        object["vtable"] = Json::array();
                        for (size_t offset = 0; offset < 80; offset += 8)
                            object["vtable"].push_back(memory_.load<Address>(table + offset));
                    }
                    objects.push_back(std::move(object));
                }
                last_fault["objects"] = std::move(objects);
            } catch (...) {
                // Diagnostics must not prevent register restoration after a guest fault.
            }
        }
        auto message = std::format("{} [pc={:#x}, lr={:#x}, import={}]", error.what(), jit.GetPC(),
                                   jit.GetRegister(30), current_import);
        restore();
        throw Error(message);
    } catch (...) {
        restore();
        throw;
    }
}
std::uint64_t Cpu::x(size_t i) const {
    return impl_->jit->GetRegister(i);
}
void Cpu::x(size_t i, std::uint64_t v) {
    impl_->jit->SetRegister(i, v);
}
double Cpu::f64(size_t i) const {
    return std::bit_cast<double>(impl_->jit->GetVector(i)[0]);
}
void Cpu::f64(size_t i, double v) {
    impl_->jit->SetVector(i, {std::bit_cast<std::uint64_t>(v), 0});
}
float Cpu::f32(size_t i) const {
    return std::bit_cast<float>(static_cast<std::uint32_t>(impl_->jit->GetVector(i)[0]));
}
void Cpu::f32(size_t i, float v) {
    impl_->jit->SetVector(i, {std::bit_cast<std::uint32_t>(v), 0});
}
std::array<std::uint64_t, 2> Cpu::v128(size_t i) const {
    return impl_->jit->GetVector(i);
}
void Cpu::v128(size_t i, std::array<std::uint64_t, 2> value) {
    impl_->jit->SetVector(i, value);
}
std::uint64_t Cpu::argument(size_t i) const {
    return i < 8 ? x(i) : memory_.load<std::uint64_t>(sp() + (i - 8) * 8);
}
Address Cpu::sp() const {
    return impl_->jit->GetSP();
}
void Cpu::sp(Address a) {
    impl_->jit->SetSP(a);
}
Address Cpu::pc() const {
    return impl_->jit->GetPC();
}
void Cpu::pc(Address a) {
    impl_->jit->SetPC(a);
}
Address Cpu::tls() const {
    return impl_->thread_pointer;
}
void Cpu::tls(Address a) {
    impl_->thread_pointer = a;
}
void Cpu::invalidate(Address a, size_t size) {
    impl_->jit->InvalidateCacheRange(a, size);
}
void Cpu::request_stop() {
    impl_->stopped = true;
    impl_->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
}
Json Cpu::statistics() const {
    const auto &p = *impl_;
    Json imports = Json::object();
    for (size_t i = 1; i < 65535; ++i)
        if (p.imports[i])
            imports[p.traps.at(static_cast<std::uint32_t>(i)).name] = p.imports[i];
    return {{"instructions", p.instructions},
            {"calls", p.calls},
            {"largest_call_instructions", p.largest_call_instructions},
            {"largest_call_function", p.largest_call_function},
            {"largest_call_elapsed_ms", p.largest_call_elapsed_ms},
            {"imports", imports},
            {"syscalls", p.imports[0]}};
}
} // namespace acp::guest
