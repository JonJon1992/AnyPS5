#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

// The values one evaluator has computed, in a table borrowed from the thread for its lifetime: a
// capture builds two evaluators, and a fresh table grew from 64 slots with a rehash per doubling
// each time. A slot belongs to the current evaluator only when it carries its generation, so
// handing the table to the next one is an increment, not a clear. The table also lends the
// evaluator its visiting stack.
class EvaluatedValues {
public:
    EvaluatedValues() : _table(Acquire()) {}
    ~EvaluatedValues() { Release(_table); }
    EvaluatedValues(const EvaluatedValues&) = delete;
    EvaluatedValues& operator=(const EvaluatedValues&) = delete;

    bool Find(const IrValue* key, std::uint64_t& value) const {
        const auto& slots = _table->slots;
        if (_table->count == 0) {
            return false;
        }
        for (std::size_t index = Home(key, slots.size());; index = (index + 1u) & (slots.size() - 1u)) {
            const auto& slot = slots[index];
            if (slot.generation != _table->generation) {
                return false;
            }
            if (slot.key == key) {
                value = slot.value;
                return true;
            }
        }
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        if ((_table->count + 1u) * 2u > _table->slots.size()) {
            Grow();
        }
        auto& slots = _table->slots;
        for (std::size_t index = Home(key, slots.size());; index = (index + 1u) & (slots.size() - 1u)) {
            auto& slot = slots[index];
            if (slot.generation != _table->generation) {
                slot = {key, value, _table->generation};
                ++_table->count;
                return;
            }
            if (slot.key == key) {
                return;
            }
        }
    }
    std::vector<IrValue*>& Visiting() { return _table->visiting; }

    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
        std::uint64_t generation = 0;
    };
    struct Table {
        std::vector<Slot> slots;
        std::size_t count = 0;
        std::uint64_t generation = 0;
        std::vector<IrValue*> visiting;
    };

private:
    static Table* Acquire();
    static void Release(Table* table);
    static std::size_t Home(const IrValue* key, std::size_t size) {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (size - 1u);
    }
    void Grow() {
        std::vector<Slot> previous(_table->slots.empty() ? 64u : _table->slots.size() * 2u);
        previous.swap(_table->slots);
        _table->count = 0;
        for (const auto& slot : previous) {
            if (slot.generation == _table->generation) {
                Insert(slot.key, slot.value);
            }
        }
    }
    Table* _table;
};

struct InaccessibleRead {
    const IrValue* read = nullptr;
    std::uint64_t address = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr) {}

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);
    void ReportInaccessibleReads(InaccessibleRead* sink) { _inaccessible = sink; }
    void ReportNullRootReads(std::uint32_t* sink) { _nullRoots = sink; }

private:
    static float Float32(std::uint64_t bits);
    static std::uint64_t Float32Bits(float value);

    bool Arg(IrValue& inst, std::size_t index, std::uint64_t& result);
    bool EvaluatePhi(IrValue& inst, std::uint64_t& result);
    bool EvaluateExtract(IrValue& inst, std::uint64_t& result);
    bool EvaluateRawRead(IrValue& inst, std::uint64_t& result);
    bool EvaluateInst(IrValue& inst, std::uint64_t& result);

    const IrResourcePlan& _program;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    Evaluator* _cleanEvaluator = nullptr;
    IrValue* _activeMask = nullptr;
    InaccessibleRead* _inaccessible = nullptr;
    std::uint32_t* _nullRoots = nullptr;
    EvaluatedValues _cache;
    std::vector<IrValue*>& _visiting = _cache.Visiting();
};

}

#endif
