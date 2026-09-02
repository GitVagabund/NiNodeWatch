#include "PCH.h"

namespace
{
    constexpr std::uintptr_t kCompactRva = 0x1CD9B0;
    constexpr std::uintptr_t kCompactExceptionSpan = 0x100;
    constexpr std::ptrdiff_t kChildrenOffset = 0x110;
    constexpr std::size_t kRingSize = 4096;
    constexpr std::size_t kDumpEventCount = 768;
    constexpr std::size_t kStackDepth = 16;
    constexpr std::uint32_t kNoIndex = (std::numeric_limits<std::uint32_t>::max)();

    enum class Operation : std::uint8_t
    {
        kAttach,
        kInsert,
        kDetachChildOut,
        kDetachChild,
        kDetachAtOut,
        kDetachAt,
        kSetAtOut,
        kSetAt,
        kCompact
    };

    enum class Phase : std::uint8_t
    {
        kEnter,
        kLeave
    };

#pragma pack(push, 1)
    struct ArrayState
    {
        std::uintptr_t vtable;
        std::uintptr_t data;
        std::uint16_t capacity;
        std::uint16_t freeIdx;
        std::uint16_t size;
        std::uint16_t growBy;
    };
#pragma pack(pop)

    static_assert(sizeof(ArrayState) == 0x18);

    struct Event
    {
        std::uint64_t sequence;
        std::uint64_t ticks;
        std::uint32_t threadId;
        Operation operation;
        Phase phase;
        std::uint8_t stateReadable;
        std::uint8_t stateSuspicious;
        std::uintptr_t node;
        std::uintptr_t array;
        std::uintptr_t child;
        std::uint32_t index;
        ArrayState state;
        char nodeName[80];
        std::uint16_t frameCount;
        std::uintptr_t frames[kStackDepth];
    };

    struct Slot
    {
        std::atomic<std::uint64_t> committed{ 0 };
        Event event{};
    };

    std::array<Slot, kRingSize> g_ring{};
    std::atomic<std::uint64_t> g_nextSequence{ 0 };
    std::atomic<std::uint32_t> g_incidentNumber{ 0 };
    std::atomic_flag g_dumping = ATOMIC_FLAG_INIT;
    thread_local std::uint32_t g_guardedReadDepth = 0;
    wchar_t g_logDirectory[MAX_PATH]{};
    PVOID g_exceptionHandler = nullptr;
    std::uintptr_t g_niNodeAsNodeTarget = 0;
    std::uintptr_t g_compactTarget = 0;

    using CompactFn = void (*)(void*);
    CompactFn g_originalCompact = nullptr;

    using AttachFn = void (*)(RE::NiNode*, RE::NiAVObject*, bool);
    using InsertFn = void (*)(RE::NiNode*, std::uint32_t, RE::NiAVObject*);
    using DetachChildOutFn = void (*)(RE::NiNode*, RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>&);
    using DetachChildFn = void (*)(RE::NiNode*, RE::NiAVObject*);
    using DetachAtOutFn = void (*)(RE::NiNode*, std::uint32_t, RE::NiPointer<RE::NiAVObject>&);
    using DetachAtFn = void (*)(RE::NiNode*, std::uint32_t);
    using SetAtOutFn = void (*)(RE::NiNode*, std::uint32_t, RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>&);
    using SetAtFn = void (*)(RE::NiNode*, std::uint32_t, RE::NiAVObject*);

    AttachFn g_originalAttach = nullptr;
    InsertFn g_originalInsert = nullptr;
    DetachChildOutFn g_originalDetachChildOut = nullptr;
    DetachChildFn g_originalDetachChild = nullptr;
    DetachAtOutFn g_originalDetachAtOut = nullptr;
    DetachAtFn g_originalDetachAt = nullptr;
    SetAtOutFn g_originalSetAtOut = nullptr;
    SetAtFn g_originalSetAt = nullptr;

    [[nodiscard]] const char* OperationName(Operation a_operation) noexcept
    {
        switch (a_operation) {
        case Operation::kAttach:
            return "AttachChild";
        case Operation::kInsert:
            return "InsertChildAt";
        case Operation::kDetachChildOut:
            return "DetachChild1";
        case Operation::kDetachChild:
            return "DetachChild2";
        case Operation::kDetachAtOut:
            return "DetachChildAt1";
        case Operation::kDetachAt:
            return "DetachChildAt2";
        case Operation::kSetAtOut:
            return "SetAt1";
        case Operation::kSetAt:
            return "SetAt2";
        case Operation::kCompact:
            return "Compact";
        default:
            return "Unknown";
        }
    }

    [[nodiscard]] const char* PhaseName(Phase a_phase) noexcept
    {
        return a_phase == Phase::kEnter ? "ENTER" : "LEAVE";
    }

    bool TryCopy(void* a_destination, const void* a_source, std::size_t a_size) noexcept
    {
        bool copied = false;
        ++g_guardedReadDepth;
        __try {
            auto* destination = static_cast<std::byte*>(a_destination);
            const auto* source = static_cast<const std::byte*>(a_source);
            for (std::size_t i = 0; i < a_size; ++i) {
                destination[i] = source[i];
            }
            copied = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            copied = false;
        }
        --g_guardedReadDepth;
        return copied;
    }

    void TryCopyNodeName(const RE::NiNode* a_node, char* a_destination, std::size_t a_capacity) noexcept
    {
        if (!a_destination || a_capacity == 0) {
            return;
        }

        a_destination[0] = '\0';
        if (!a_node) {
            return;
        }

        ++g_guardedReadDepth;
        __try {
            const char* source = a_node->name.c_str();
            if (source) {
                std::size_t i = 0;
                for (; i + 1 < a_capacity && source[i] != '\0'; ++i) {
                    const auto value = static_cast<unsigned char>(source[i]);
                    a_destination[i] = value >= 0x20 ? static_cast<char>(value) : '?';
                }
                a_destination[i] = '\0';
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a_destination[0] = '\0';
        }
        --g_guardedReadDepth;
    }

    [[nodiscard]] bool IsReadableAddress(std::uintptr_t a_address, std::size_t a_size) noexcept
    {
        if (a_address == 0 || a_size == 0) {
            return false;
        }

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(a_address), &info, sizeof(info)) != sizeof(info)) {
            return false;
        }

        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        const DWORD protection = info.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                              protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable) {
            return false;
        }

        const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto regionEnd = regionStart + info.RegionSize;
        return a_address >= regionStart && a_address <= regionEnd && a_size <= regionEnd - a_address;
    }

    [[nodiscard]] bool IsSuspicious(const ArrayState& a_state, bool a_readable) noexcept
    {
        if (!a_readable) {
            return true;
        }
        if (a_state.size > a_state.capacity || a_state.freeIdx > a_state.capacity) {
            return true;
        }
        if (a_state.capacity != 0 && a_state.data == 0) {
            return true;
        }
        if (a_state.data != 0) {
            if ((a_state.data & (alignof(void*) - 1)) != 0) {
                return true;
            }
            const auto entries = std::max<std::size_t>(a_state.capacity, 1);
            const auto bytes = std::min<std::size_t>(entries * sizeof(void*), 64);
            if (!IsReadableAddress(a_state.data, bytes)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Event MakeEvent(
        Operation a_operation,
        Phase a_phase,
        RE::NiNode* a_node,
        RE::NiAVObject* a_child,
        std::uint32_t a_index,
        const void* a_arrayOverride = nullptr) noexcept
    {
        Event event{};
        event.ticks = GetTickCount64();
        event.threadId = GetCurrentThreadId();
        event.operation = a_operation;
        event.phase = a_phase;
        event.node = reinterpret_cast<std::uintptr_t>(a_node);
        event.child = reinterpret_cast<std::uintptr_t>(a_child);
        event.index = a_index;

        const void* arrayAddress = a_arrayOverride;
        if (!arrayAddress && a_node) {
            arrayAddress = reinterpret_cast<const std::byte*>(a_node) + kChildrenOffset;
        }
        event.array = reinterpret_cast<std::uintptr_t>(arrayAddress);

        event.stateReadable = TryCopy(&event.state, arrayAddress, sizeof(event.state)) ? 1 : 0;
        event.stateSuspicious = IsSuspicious(event.state, event.stateReadable != 0) ? 1 : 0;
        TryCopyNodeName(a_node, event.nodeName, sizeof(event.nodeName));

        event.frameCount = RtlCaptureStackBackTrace(
            1,
            static_cast<DWORD>(kStackDepth),
            reinterpret_cast<PVOID*>(event.frames),
            nullptr);
        return event;
    }

    std::uint64_t Record(
        Operation a_operation,
        Phase a_phase,
        RE::NiNode* a_node,
        RE::NiAVObject* a_child = nullptr,
        std::uint32_t a_index = kNoIndex,
        const void* a_arrayOverride = nullptr) noexcept
    {
        const auto sequence = g_nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        auto& slot = g_ring[(sequence - 1) % kRingSize];
        slot.committed.store(0, std::memory_order_relaxed);

        Event event = MakeEvent(a_operation, a_phase, a_node, a_child, a_index, a_arrayOverride);
        event.sequence = sequence;
        slot.event = event;
        slot.committed.store(sequence, std::memory_order_release);
        return sequence;
    }

    void WriteText(HANDLE a_file, const char* a_text, std::size_t a_length) noexcept
    {
        while (a_length != 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(a_length, 0x7FFFFFFF));
            DWORD written = 0;
            if (!WriteFile(a_file, a_text, chunk, &written, nullptr) || written == 0) {
                return;
            }
            a_text += written;
            a_length -= written;
        }
    }

    template <class... Args>
    void WriteFormat(HANDLE a_file, const char* a_format, Args... a_args) noexcept
    {
        char buffer[1024]{};
        const int length = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, a_format, a_args...);
        if (length > 0) {
            WriteText(a_file, buffer, static_cast<std::size_t>(length));
        }
    }

    void WriteAddress(HANDLE a_file, std::uintptr_t a_address) noexcept
    {
        HMODULE module = nullptr;
        constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(a_address), &module)) {
            char path[MAX_PATH]{};
            if (GetModuleFileNameA(module, path, MAX_PATH) != 0) {
                const char* name = path;
                for (const char* cursor = path; *cursor != '\0'; ++cursor) {
                    if (*cursor == '\\' || *cursor == '/') {
                        name = cursor + 1;
                    }
                }
                const auto base = reinterpret_cast<std::uintptr_t>(module);
                WriteFormat(a_file, "%s+0x%llX", name,
                    static_cast<unsigned long long>(a_address - base));
                return;
            }
        }

        WriteFormat(a_file, "0x%llX", static_cast<unsigned long long>(a_address));
    }

    void WriteEvent(HANDLE a_file, const Event& a_event, std::uintptr_t a_focusNode) noexcept
    {
        WriteFormat(a_file,
            "\r\n[#%llu t=%llu tid=%lu] %s %s%s\r\n"
            "  node=0x%llX array=0x%llX name=\"%s\" child=0x%llX index=",
            static_cast<unsigned long long>(a_event.sequence),
            static_cast<unsigned long long>(a_event.ticks),
            static_cast<unsigned long>(a_event.threadId),
            OperationName(a_event.operation),
            PhaseName(a_event.phase),
            a_event.node == a_focusNode && a_focusNode != 0 ? " SAME_NODE" : "",
            static_cast<unsigned long long>(a_event.node),
            static_cast<unsigned long long>(a_event.array),
            a_event.nodeName[0] != '\0' ? a_event.nodeName : "<unavailable>",
            static_cast<unsigned long long>(a_event.child));

        if (a_event.index == kNoIndex) {
            WriteText(a_file, "-\r\n", 3);
        } else {
            WriteFormat(a_file, "%lu\r\n", static_cast<unsigned long>(a_event.index));
        }

        WriteFormat(a_file,
            "  children: readable=%u suspicious=%u vtable=0x%llX data=0x%llX "
            "capacity=%u freeIdx=%u size=%u growBy=%u\r\n",
            static_cast<unsigned>(a_event.stateReadable),
            static_cast<unsigned>(a_event.stateSuspicious),
            static_cast<unsigned long long>(a_event.state.vtable),
            static_cast<unsigned long long>(a_event.state.data),
            static_cast<unsigned>(a_event.state.capacity),
            static_cast<unsigned>(a_event.state.freeIdx),
            static_cast<unsigned>(a_event.state.size),
            static_cast<unsigned>(a_event.state.growBy));

        WriteText(a_file, "  stack:\r\n", 10);
        for (std::uint16_t i = 0; i < a_event.frameCount && i < kStackDepth; ++i) {
            WriteText(a_file, "    ", 4);
            WriteAddress(a_file, a_event.frames[i]);
            WriteText(a_file, "\r\n", 2);
        }
    }

    void DumpIncident(const char* a_reason, std::uintptr_t a_focusNode, EXCEPTION_POINTERS* a_exception) noexcept
    {
        if (g_dumping.test_and_set(std::memory_order_acquire)) {
            return;
        }

        SYSTEMTIME time{};
        GetLocalTime(&time);
        const auto incident = g_incidentNumber.fetch_add(1, std::memory_order_relaxed) + 1;

        wchar_t path[MAX_PATH]{};
        _snwprintf_s(path, MAX_PATH, _TRUNCATE,
            L"%s\\NiNodeWatch-incident-%04u%02u%02u-%02u%02u%02u-T%lu-%lu.log",
            g_logDirectory,
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            static_cast<unsigned long>(GetCurrentThreadId()),
            static_cast<unsigned long>(incident));

        HANDLE file = CreateFileW(
            path,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) {
            g_dumping.clear(std::memory_order_release);
            return;
        }

        constexpr char header[] = "NiNodeWatch 0.1.1 / Skyrim SE 1.5.97 only\r\n";
        WriteText(file, header, sizeof(header) - 1);
        WriteFormat(file, "reason=%s\r\nfocusNode=0x%llX\r\n",
            a_reason ? a_reason : "unknown",
            static_cast<unsigned long long>(a_focusNode));

        if (a_exception && a_exception->ExceptionRecord) {
            WriteFormat(file, "exception=0x%08lX address=",
                static_cast<unsigned long>(a_exception->ExceptionRecord->ExceptionCode));
            WriteAddress(file,
                reinterpret_cast<std::uintptr_t>(a_exception->ExceptionRecord->ExceptionAddress));
            WriteText(file, "\r\n", 2);
        }

        const auto latest = g_nextSequence.load(std::memory_order_acquire);
        const auto first = latest > kDumpEventCount ? latest - kDumpEventCount + 1 : 1;
        WriteFormat(file, "events=%llu..%llu (ring capacity=%llu)\r\n",
            static_cast<unsigned long long>(first),
            static_cast<unsigned long long>(latest),
            static_cast<unsigned long long>(kRingSize));

        for (std::uint64_t sequence = first; sequence <= latest; ++sequence) {
            const auto& slot = g_ring[(sequence - 1) % kRingSize];
            if (slot.committed.load(std::memory_order_acquire) != sequence) {
                continue;
            }

            Event event{};
            if (!TryCopy(&event, &slot.event, sizeof(event))) {
                continue;
            }
            if (slot.committed.load(std::memory_order_acquire) != sequence || event.sequence != sequence) {
                continue;
            }
            WriteEvent(file, event, a_focusNode);
        }

        FlushFileBuffers(file);
        CloseHandle(file);
        g_dumping.clear(std::memory_order_release);
    }

    LONG CALLBACK ExceptionHandler(EXCEPTION_POINTERS* a_exception) noexcept
    {
        if (g_guardedReadDepth != 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (!a_exception || !a_exception->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = a_exception->ExceptionRecord->ExceptionCode;
        const auto exceptionAddress = reinterpret_cast<std::uintptr_t>(
            a_exception->ExceptionRecord->ExceptionAddress);

        // Many SKSE plugins deliberately probe memory under SEH. Those first-chance
        // access violations are expected and may be handled by the originating plugin.
        // Only capture the exact Compact routine implicated by the original crash.
        if (code != EXCEPTION_ACCESS_VIOLATION || g_compactTarget == 0 ||
            exceptionAddress < g_compactTarget ||
            exceptionAddress >= g_compactTarget + kCompactExceptionSpan) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        std::uintptr_t focusNode = 0;
        const auto latest = g_nextSequence.load(std::memory_order_acquire);
        const auto first = latest > 64 ? latest - 63 : 1;
        const auto threadId = GetCurrentThreadId();
        for (std::uint64_t sequence = latest; sequence >= first && sequence != 0; --sequence) {
            const auto& slot = g_ring[(sequence - 1) % kRingSize];
            if (slot.committed.load(std::memory_order_acquire) == sequence &&
                slot.event.threadId == threadId && slot.event.node != 0) {
                focusNode = slot.event.node;
                if (slot.event.stateSuspicious != 0 || slot.event.operation == Operation::kCompact) {
                    break;
                }
            }
        }
        DumpIncident("access violation in NiTObjectArray::Compact", focusNode, a_exception);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void HookAttach(RE::NiNode* a_node, RE::NiAVObject* a_child, bool a_firstAvailable)
    {
        Record(Operation::kAttach, Phase::kEnter, a_node, a_child);
        g_originalAttach(a_node, a_child, a_firstAvailable);
        Record(Operation::kAttach, Phase::kLeave, a_node, a_child);
    }

    void HookInsert(RE::NiNode* a_node, std::uint32_t a_index, RE::NiAVObject* a_child)
    {
        Record(Operation::kInsert, Phase::kEnter, a_node, a_child, a_index);
        g_originalInsert(a_node, a_index, a_child);
        Record(Operation::kInsert, Phase::kLeave, a_node, a_child, a_index);
    }

    void HookDetachChildOut(
        RE::NiNode* a_node,
        RE::NiAVObject* a_child,
        RE::NiPointer<RE::NiAVObject>& a_childOut)
    {
        Record(Operation::kDetachChildOut, Phase::kEnter, a_node, a_child);
        g_originalDetachChildOut(a_node, a_child, a_childOut);
        Record(Operation::kDetachChildOut, Phase::kLeave, a_node, a_child);
    }

    void HookDetachChild(RE::NiNode* a_node, RE::NiAVObject* a_child)
    {
        Record(Operation::kDetachChild, Phase::kEnter, a_node, a_child);
        g_originalDetachChild(a_node, a_child);
        Record(Operation::kDetachChild, Phase::kLeave, a_node, a_child);
    }

    void HookDetachAtOut(
        RE::NiNode* a_node,
        std::uint32_t a_index,
        RE::NiPointer<RE::NiAVObject>& a_childOut)
    {
        Record(Operation::kDetachAtOut, Phase::kEnter, a_node, nullptr, a_index);
        g_originalDetachAtOut(a_node, a_index, a_childOut);
        Record(Operation::kDetachAtOut, Phase::kLeave, a_node, a_childOut.get(), a_index);
    }

    void HookDetachAt(RE::NiNode* a_node, std::uint32_t a_index)
    {
        Record(Operation::kDetachAt, Phase::kEnter, a_node, nullptr, a_index);
        g_originalDetachAt(a_node, a_index);
        Record(Operation::kDetachAt, Phase::kLeave, a_node, nullptr, a_index);
    }

    void HookSetAtOut(
        RE::NiNode* a_node,
        std::uint32_t a_index,
        RE::NiAVObject* a_child,
        RE::NiPointer<RE::NiAVObject>& a_childOut)
    {
        Record(Operation::kSetAtOut, Phase::kEnter, a_node, a_child, a_index);
        g_originalSetAtOut(a_node, a_index, a_child, a_childOut);
        Record(Operation::kSetAtOut, Phase::kLeave, a_node, a_child, a_index);
    }

    void HookSetAt(RE::NiNode* a_node, std::uint32_t a_index, RE::NiAVObject* a_child)
    {
        Record(Operation::kSetAt, Phase::kEnter, a_node, a_child, a_index);
        g_originalSetAt(a_node, a_index, a_child);
        Record(Operation::kSetAt, Phase::kLeave, a_node, a_child, a_index);
    }

    void HookCompact(void* a_array)
    {
        const auto arrayAddress = reinterpret_cast<std::uintptr_t>(a_array);
        if (arrayAddress < static_cast<std::uintptr_t>(kChildrenOffset)) {
            g_originalCompact(a_array);
            return;
        }

        auto* node = reinterpret_cast<RE::NiNode*>(arrayAddress - kChildrenOffset);
        std::uintptr_t candidateVtable = 0;
        std::uintptr_t candidateAsNode = 0;
        if (!TryCopy(&candidateVtable, node, sizeof(candidateVtable)) ||
            !TryCopy(&candidateAsNode,
                reinterpret_cast<const void*>(candidateVtable + 0x03 * sizeof(void*)),
                sizeof(candidateAsNode)) ||
            candidateAsNode != g_niNodeAsNodeTarget) {
            g_originalCompact(a_array);
            return;
        }

        const auto sequence = Record(
            Operation::kCompact, Phase::kEnter, node, nullptr, kNoIndex, a_array);

        const auto& slot = g_ring[(sequence - 1) % kRingSize];
        if (slot.committed.load(std::memory_order_acquire) == sequence && slot.event.stateSuspicious != 0) {
            DumpIncident("suspicious children state before Compact",
                reinterpret_cast<std::uintptr_t>(node), nullptr);
        }

        g_originalCompact(a_array);
        Record(Operation::kCompact, Phase::kLeave, node, nullptr, kNoIndex, a_array);
    }

    [[nodiscard]] bool CreateHook(void* a_target, void* a_detour, void** a_original, const char* a_name)
    {
        const MH_STATUS status = MH_CreateHook(a_target, a_detour, a_original);
        if (status != MH_OK) {
            SKSE::log::critical("MH_CreateHook({}) failed: {}", a_name, MH_StatusToString(status));
            return false;
        }
        SKSE::log::info("hook prepared: {} target={}", a_name, a_target);
        return true;
    }

    [[nodiscard]] void* VTableTarget(std::uintptr_t a_vtable, std::size_t a_slot) noexcept
    {
        void* target = nullptr;
        TryCopy(&target, reinterpret_cast<const void*>(a_vtable + a_slot * sizeof(void*)), sizeof(target));
        return target;
    }

    [[nodiscard]] bool InstallHooks()
    {
        if (MH_Initialize() != MH_OK) {
            SKSE::log::critical("MH_Initialize failed");
            return false;
        }

        REL::Relocation<std::uintptr_t> niNodeVtable{ RE::VTABLE_NiNode[0] };
        const auto vtable = niNodeVtable.address();
        const auto imageBase = REL::Module::get().base();
        g_compactTarget = imageBase + kCompactRva;
        g_niNodeAsNodeTarget = reinterpret_cast<std::uintptr_t>(VTableTarget(vtable, 0x03));
        if (g_niNodeAsNodeTarget == 0) {
            SKSE::log::critical("could not resolve NiNode::AsNode from vtable");
            MH_Uninitialize();
            return false;
        }

        struct HookSpec
        {
            void* target;
            void* detour;
            void** original;
            const char* name;
        };

        const std::array mutationHooks{
            HookSpec{ VTableTarget(vtable, 0x35), reinterpret_cast<void*>(&HookAttach), reinterpret_cast<void**>(&g_originalAttach), "NiNode::AttachChild" },
            HookSpec{ VTableTarget(vtable, 0x36), reinterpret_cast<void*>(&HookInsert), reinterpret_cast<void**>(&g_originalInsert), "NiNode::InsertChildAt" },
            HookSpec{ VTableTarget(vtable, 0x37), reinterpret_cast<void*>(&HookDetachChildOut), reinterpret_cast<void**>(&g_originalDetachChildOut), "NiNode::DetachChild1" },
            HookSpec{ VTableTarget(vtable, 0x38), reinterpret_cast<void*>(&HookDetachChild), reinterpret_cast<void**>(&g_originalDetachChild), "NiNode::DetachChild2" },
            HookSpec{ VTableTarget(vtable, 0x39), reinterpret_cast<void*>(&HookDetachAtOut), reinterpret_cast<void**>(&g_originalDetachAtOut), "NiNode::DetachChildAt1" },
            HookSpec{ VTableTarget(vtable, 0x3A), reinterpret_cast<void*>(&HookDetachAt), reinterpret_cast<void**>(&g_originalDetachAt), "NiNode::DetachChildAt2" },
            HookSpec{ VTableTarget(vtable, 0x3B), reinterpret_cast<void*>(&HookSetAtOut), reinterpret_cast<void**>(&g_originalSetAtOut), "NiNode::SetAt1" },
            HookSpec{ VTableTarget(vtable, 0x3C), reinterpret_cast<void*>(&HookSetAt), reinterpret_cast<void**>(&g_originalSetAt), "NiNode::SetAt2" }
        };

        std::size_t mutationHookCount = 0;
        for (const auto& hook : mutationHooks) {
            if (!hook.target) {
                SKSE::log::error("could not resolve optional hook {}", hook.name);
                continue;
            }
            mutationHookCount += CreateHook(hook.target, hook.detour, hook.original, hook.name) ? 1 : 0;
        }

        const HookSpec compactHook{
            reinterpret_cast<void*>(g_compactTarget),
            reinterpret_cast<void*>(&HookCompact),
            reinterpret_cast<void**>(&g_originalCompact),
            "NiTObjectArray::Compact@1CD9B0"
        };
        if (!CreateHook(compactHook.target, compactHook.detour, compactHook.original, compactHook.name)) {
            SKSE::log::critical("the required Compact hook could not be installed");
            MH_Uninitialize();
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK) {
            SKSE::log::critical("MH_EnableHook failed: {}", MH_StatusToString(enableStatus));
            MH_Uninitialize();
            return false;
        }

        g_exceptionHandler = AddVectoredExceptionHandler(1, ExceptionHandler);
        SKSE::log::info(
            "NiNodeWatch hooks installed; mutation hooks={}/{} Compact RVA=0x{:X}",
            mutationHookCount, mutationHooks.size(), kCompactRva);
        return true;
    }

    void SetupLogging()
    {
        const auto directory = SKSE::log::log_directory();
        if (directory) {
            std::filesystem::create_directories(*directory);
            wcsncpy_s(g_logDirectory, directory->c_str(), _TRUNCATE);

            auto logPath = *directory / "NiNodeWatch.log";
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
            auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
        } else {
            GetCurrentDirectoryW(MAX_PATH, g_logDirectory);
        }
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            if (!InstallHooks()) {
                SKSE::log::critical("NiNodeWatch disabled because hook installation failed");
            }
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLogging();

    const REL::Version expected{ 1, 5, 97, 0 };
    const auto actual = REL::Module::get().version();
    if (actual != expected) {
        SKSE::log::critical(
            "NiNodeWatch supports only Skyrim SE 1.5.97.0; current runtime is {}",
            actual.string());
        return false;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener("SKSE", MessageHandler)) {
        SKSE::log::critical("failed to register SKSE message listener");
        return false;
    }

    SKSE::log::info("NiNodeWatch 0.1.1 loaded; waiting for DataLoaded");
    return true;
}
