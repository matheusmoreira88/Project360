#include "kernel/guest_executor.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace project360::kernel {
namespace {

[[nodiscard]] std::string dispatch_error(KernelDispatchStatus status,
                                         const loader::XexImportLibrary& library,
                                         std::uint16_t ordinal) {
    const auto prefix = "kernel import " + library.name + " ordinal " +
                        std::to_string(ordinal) + ": ";
    switch (status) {
    case KernelDispatchStatus::handled:
        return {};
    case KernelDispatchStatus::unsupported_library:
        return prefix + "library is unsupported";
    case KernelDispatchStatus::unsupported_export:
        return prefix + "export is not implemented";
    case KernelDispatchStatus::invalid_guest_argument:
        return prefix + "guest arguments are invalid";
    case KernelDispatchStatus::guest_memory_fault:
        return prefix + "guest memory access failed";
    }
    return prefix + "dispatcher returned an unknown status";
}

[[nodiscard]] cpu::StepResult execution_fault(std::string message) {
    return {cpu::StepStatus::fault, std::move(message), std::nullopt, std::nullopt};
}

} // namespace

GuestExecutor::GuestExecutor(const loader::LoadedXexImage& image) : image_(image) {
    try {
        std::size_t total_imports = 0;
        for (const auto& library : image_.xex.import_libraries) {
            if (library.import_record_addresses.size() >
                std::numeric_limits<std::size_t>::max() - total_imports) {
                initialization_error_ = "XEX import count exceeds host addressable memory";
                return;
            }
            total_imports += library.import_record_addresses.size();
        }
        import_addresses_.reserve(total_imports);
        for (std::size_t library_index = 0;
             library_index < image_.xex.import_libraries.size(); ++library_index) {
            const auto& library = image_.xex.import_libraries[library_index];
            if (library.import_types.size() != library.import_record_addresses.size() ||
                library.import_ordinals.size() != library.import_record_addresses.size()) {
                initialization_error_ = "loaded XEX has undecoded import records";
                import_addresses_.clear();
                return;
            }
            for (std::size_t import_index = 0;
                 import_index < library.import_record_addresses.size(); ++import_index) {
                if (!library.import_types[import_index] || !library.import_ordinals[import_index]) {
                    initialization_error_ = "loaded XEX has an incomplete import record";
                    import_addresses_.clear();
                    return;
                }
                import_addresses_.push_back({
                    library.import_record_addresses[import_index],
                    static_cast<std::uint32_t>(library_index),
                    static_cast<std::uint32_t>(import_index),
                });
            }
        }
    } catch (const std::bad_alloc&) {
        initialization_error_ = "not enough host memory for the XEX import dispatch index";
        import_addresses_.clear();
        return;
    }
    std::sort(import_addresses_.begin(), import_addresses_.end(),
              [](const ImportAddress& left, const ImportAddress& right) {
                  return left.address < right.address;
              });
    const auto duplicate = std::adjacent_find(
        import_addresses_.begin(), import_addresses_.end(),
        [](const ImportAddress& left, const ImportAddress& right) {
            return left.address == right.address;
        });
    if (duplicate != import_addresses_.end()) {
        initialization_error_ = "loaded XEX has duplicate import thunk addresses";
        import_addresses_.clear();
    }
}

cpu::StepResult GuestExecutor::step(cpu::CpuState& state,
                                    memory::GuestMemory& memory) const {
    if (!initialization_error_.empty()) return execution_fault(initialization_error_);

    const auto import = std::lower_bound(
        import_addresses_.begin(), import_addresses_.end(), state.pc,
        [](const ImportAddress& candidate, std::uint64_t address) {
            return candidate.address < address;
        });
    if (import == import_addresses_.end() || import->address != state.pc) {
        return interpreter_.step(state, memory);
    }

    const auto& library = image_.xex.import_libraries[import->library_index];
    const auto import_index = static_cast<std::size_t>(import->import_index);
    const auto type = *library.import_types[import_index];
    const auto ordinal = *library.import_ordinals[import_index];
    if (type == 0U) {
        return execution_fault("guest attempted to execute a data import from " +
                               library.name);
    }

    const auto status = dispatcher_.dispatch({library.name, ordinal}, state, memory);
    if (status != KernelDispatchStatus::handled) {
        return execution_fault(dispatch_error(status, library, ordinal));
    }
    return {cpu::StepStatus::executed,
            "dispatched kernel import " + library.name + " ordinal " +
                std::to_string(ordinal),
            std::nullopt, std::nullopt};
}

} // namespace project360::kernel
