#include "core.hpp"
#include "protocol.hpp"

#include <bluetoothleapis.h>
#include <setupapi.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace m720 {
namespace {

using namespace hidpp;
using namespace std::chrono_literals;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;
constexpr DWORD retry_ms = 2500;
constexpr DWORD reply_ms = 3000;
constexpr auto more_data = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
constexpr GUID ble_device{0x781aee18, 0x7733, 0x4ce4, {0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92}};
constexpr GUID gatt_service{0x6e3bb679, 0x4372, 0x40c8, {0x9e, 0xaa, 0x45, 0x09, 0xdf, 0x26, 0x0c, 0xd8}};
constexpr GUID logitech_service{0x00010000, 0, 0x1000, {0x80, 0, 0x01, 0x1f, 0x20, 0, 0x04, 0x6d}};
constexpr GUID logitech_value{0x00010001, 0, 0x1000, {0x80, 0, 0x01, 0x1f, 0x20, 0, 0x04, 0x6d}};
constexpr wchar_t address_property[] = L"System.Devices.Aep.DeviceAddress";
constexpr wchar_t connected_property[] = L"System.Devices.Aep.IsConnected";

struct TransportError : std::runtime_error { using runtime_error::runtime_error; };
struct RestoreError : std::runtime_error { using runtime_error::runtime_error; };

void check_gatt(HRESULT result, std::string_view operation) {
    if (result != S_OK) throw TransportError(std::format("{} failed (0x{:08X}).", operation, static_cast<unsigned>(result)));
}

winrt::handle event(bool manual_reset) {
    winrt::handle value{CreateEventW(nullptr, manual_reset, FALSE, nullptr)};
    winrt::check_bool(static_cast<bool>(value));
    return value;
}

bool signaled(HANDLE value) {
    const auto status = WaitForSingleObject(value, 0);
    if (status == WAIT_FAILED) winrt::throw_last_error();
    return status == WAIT_OBJECT_0;
}

DWORD wait_any(std::span<const HANDLE> handles, DWORD ms = INFINITE) {
    const auto status = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, ms);
    if (status == WAIT_FAILED) winrt::throw_last_error();
    return status;
}

std::wstring lower_address(std::wstring_view address) {
    std::wstring normalized;
    for (wchar_t c : address) {
        if (c == L':') continue;
        if (c >= L'A' && c <= L'F') c += L'a' - L'A';
        if ((c < L'0' || c > L'9') && (c < L'a' || c > L'f')) return {};
        normalized.push_back(c);
    }
    return normalized.size() == 12 ? normalized : std::wstring{};
}

std::wstring path_address(std::wstring_view path) {
    const auto product = path.find(L"dev_vid&02046d_pid&b015");
    if (product == path.npos) return {};
    const auto end = path.find(L'#', product);
    if (end == path.npos || end < 13 || path[end - 13] != L'_') return {};
    return lower_address(path.substr(end - 12, 12));
}

struct Link {
    explicit Link(std::wstring device_address) : address(std::move(device_address)) {}
    bool lost() const { return signaled(lost_event.get()); }

    std::wstring address;
    winrt::handle lost_event{event(true)};
};

struct MonitorState {
    winrt::handle changed{event(false)};
    std::mutex mutex;
    struct Endpoint { std::wstring address; bool model_hint; };
    std::unordered_map<std::wstring, Endpoint> endpoints;
    std::unordered_set<std::wstring> online;
    std::weak_ptr<Link> active;
    bool stopped{};
    bool enumerated{};

    void update_locked(const std::wstring& address, bool connected) {
        if (connected ? !online.insert(address).second : online.erase(address) == 0) return;
        if (!connected) {
            if (auto link = active.lock(); link && link->address == address) SetEvent(link->lost_event.get());
        }
        SetEvent(changed.get());
    }

    void stop() {
        std::scoped_lock lock(mutex);
        stopped = true;
        if (auto link = active.lock()) SetEvent(link->lost_event.get());
        SetEvent(changed.get());
    }

    std::shared_ptr<Link> acquire(std::wstring address) {
        std::scoped_lock lock(mutex);
        if (stopped || !online.contains(address)) return {};
        auto link = std::make_shared<Link>(std::move(address));
        active = link;
        return link;
    }

    bool model_online() {
        std::scoped_lock lock(mutex);
        return std::any_of(endpoints.begin(), endpoints.end(), [this](const auto& item) {
            return item.second.model_hint && online.contains(item.second.address);
        });
    }

    bool initial_enumeration_done() {
        std::scoped_lock lock(mutex);
        return enumerated;
    }

    bool is_stopped() {
        std::scoped_lock lock(mutex);
        return stopped;
    }
};

class Monitor {
public:
    Monitor() {
        auto properties = winrt::single_threaded_vector<winrt::hstring>();
        properties.Append(address_property);
        properties.Append(connected_property);
        watcher_ = DeviceInformation::CreateWatcher(
            BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), properties, DeviceInformationKind::AssociationEndpoint);
        const auto state = state_;
        added_ = watcher_.Added([state](const DeviceWatcher&, const DeviceInformation& info) {
            try {
                const auto properties = info.Properties();
                if (!properties.HasKey(address_property)) return;
                const auto address_value = winrt::unbox_value_or<winrt::hstring>(properties.Lookup(address_property), winrt::hstring{});
                auto address = lower_address(std::wstring_view(address_value));
                if (address.empty()) return;
                std::scoped_lock lock(state->mutex);
                if (state->stopped) return;
                const auto name = info.Name();
                const bool model_hint = std::wstring_view(name).find(L"M720") != std::wstring_view::npos;
                state->endpoints.insert_or_assign(std::wstring(info.Id().c_str()), MonitorState::Endpoint{address, model_hint});
                if (properties.HasKey(connected_property))
                    state->update_locked(address, winrt::unbox_value_or<bool>(properties.Lookup(connected_property), false));
            } catch (...) { state->stop(); }
        });
        updated_ = watcher_.Updated([state](const DeviceWatcher&, const DeviceInformationUpdate& info) {
            try {
                const auto properties = info.Properties();
                if (!properties.HasKey(connected_property)) return;
                const bool connected = winrt::unbox_value_or<bool>(properties.Lookup(connected_property), false);
                std::scoped_lock lock(state->mutex);
                const auto entry = state->endpoints.find(std::wstring(info.Id().c_str()));
                if (!state->stopped && entry != state->endpoints.end()) state->update_locked(entry->second.address, connected);
            } catch (...) { state->stop(); }
        });
        removed_ = watcher_.Removed([state](const DeviceWatcher&, const DeviceInformationUpdate& info) {
            try {
                std::scoped_lock lock(state->mutex);
                const auto entry = state->endpoints.find(std::wstring(info.Id().c_str()));
                if (!state->stopped && entry != state->endpoints.end()) {
                    state->update_locked(entry->second.address, false);
                    state->endpoints.erase(entry);
                }
            } catch (...) { state->stop(); }
        });
        enumerated_ = watcher_.EnumerationCompleted([state](const DeviceWatcher&, const auto&) {
            std::scoped_lock lock(state->mutex);
            state->enumerated = true;
            SetEvent(state->changed.get());
        });
        stopped_ = watcher_.Stopped([state](const DeviceWatcher&, const auto&) { state->stop(); });
        watcher_.Start();
    }

    ~Monitor() {
        watcher_.Added(added_);
        watcher_.Updated(updated_);
        watcher_.Removed(removed_);
        watcher_.Stopped(stopped_);
        watcher_.EnumerationCompleted(enumerated_);
        try {
            const auto status = watcher_.Status();
            if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted)
                watcher_.Stop();
        } catch (const winrt::hresult_error&) {}
    }

    MonitorState& state() const { return *state_; }

private:
    std::shared_ptr<MonitorState> state_ = std::make_shared<MonitorState>();
    DeviceWatcher watcher_{nullptr};
    winrt::event_token added_{}, updated_{}, removed_{}, stopped_{}, enumerated_{};
};

struct DeviceSet {
    explicit DeviceSet(const GUID& type) : handle(SetupDiGetClassDevsW(&type, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)) {}
    ~DeviceSet() { if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle); }
    HDEVINFO handle;
};

std::vector<std::max_align_t> storage(std::size_t bytes) {
    return std::vector<std::max_align_t>((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
}

std::vector<std::wstring> candidate_paths() {
    std::vector<std::wstring> paths;
    for (const auto& type : {ble_device, gatt_service}) {
        DeviceSet set(type);
        if (set.handle == INVALID_HANDLE_VALUE) continue;
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA data{};
            data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInterfaces(set.handle, nullptr, &type, index, &data)) {
                if (GetLastError() != ERROR_NO_MORE_ITEMS) winrt::throw_last_error();
                break;
            }

            DWORD required{};
            SetupDiGetDeviceInterfaceDetailW(set.handle, &data, nullptr, 0, &required, nullptr);
            if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            auto buffer = storage(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(*detail);
            if (!SetupDiGetDeviceInterfaceDetailW(set.handle, &data, detail, required, nullptr, nullptr)) continue;
            std::wstring path(detail->DevicePath);
            std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) {
                return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
            });
            if (path.find(L"bthledevice#") != path.npos && !path_address(path).empty())
                paths.push_back(std::move(path));
        }
    }
    return paths;
}

winrt::handle open_device(const std::wstring& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    return winrt::handle{handle == INVALID_HANDLE_VALUE ? nullptr : handle};
}

bool uuid_is(const BTH_LE_UUID& uuid, const GUID& match) {
    return !uuid.IsShortUuid && uuid.Value.LongUuid == match;
}

std::vector<BTH_LE_GATT_SERVICE> services(HANDLE device) {
    USHORT count{};
    if (BluetoothGATTGetServices(device, 0, nullptr, &count, 0) != more_data || !count) return {};
    std::vector<BTH_LE_GATT_SERVICE> items(count);
    if (BluetoothGATTGetServices(device, count, items.data(), &count, 0) != S_OK) return {};
    items.resize(count);
    return items;
}

std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(HANDLE device, BTH_LE_GATT_SERVICE& service) {
    USHORT count{};
    if (BluetoothGATTGetCharacteristics(device, &service, 0, nullptr, &count, 0) != more_data || !count) return {};
    std::vector<BTH_LE_GATT_CHARACTERISTIC> items(count);
    if (BluetoothGATTGetCharacteristics(device, &service, count, items.data(), &count, 0) != S_OK) return {};
    items.resize(count);
    return items;
}

std::optional<BTH_LE_GATT_CHARACTERISTIC> find_characteristic(HANDLE device) {
    for (auto& service : services(device)) {
        if (!uuid_is(service.ServiceUuid, logitech_service)) continue;
        for (const auto& value : characteristics(device, service)) {
            if (uuid_is(value.CharacteristicUuid, logitech_value) && value.IsWritable && value.IsNotifiable)
                return value;
        }
    }
    return std::nullopt;
}

struct Notifications {
    winrt::handle reply_event{event(false)};
    std::shared_ptr<Link> link;
    std::mutex mutex;
    Byte feature{}, address{};
    std::optional<Packet> reply;
    std::function<void(const Packet&)> listener;
};

// The native callback receives a registration token, never an owning object pointer.
// A notification already queued at teardown cannot dereference a freed transport.
std::mutex callback_mutex;
std::unordered_map<std::uintptr_t, std::weak_ptr<Notifications>> callbacks;
std::atomic<std::uintptr_t> next_callback{1};

void CALLBACK notification(BTH_LE_GATT_EVENT_TYPE type, PVOID data, PVOID context) noexcept {
    try {
        if (type != CharacteristicValueChangedEvent || !data) return;
        const auto& changed = *static_cast<BLUETOOTH_GATT_VALUE_CHANGED_EVENT*>(data);
        if (!changed.CharacteristicValue || changed.CharacteristicValue->DataSize != Packet{}.size()) return;

        std::shared_ptr<Notifications> state;
        {
            std::scoped_lock lock(callback_mutex);
            const auto entry = callbacks.find(reinterpret_cast<std::uintptr_t>(context));
            if (entry != callbacks.end()) state = entry->second.lock();
        }
        if (!state) return;
        Packet packet;
        std::memcpy(packet.data(), changed.CharacteristicValue->Data, packet.size());

        std::scoped_lock lock(state->mutex);
        if (state->link && state->link->lost()) return;
        if (state->address && !state->reply && classify(packet, state->feature, state->address) != Reply::unrelated) {
            state->reply = packet;
            SetEvent(state->reply_event.get());
        } else if ((packet[1] & 0x0F) == 0 && state->listener) {
            state->listener(packet);
        }
    } catch (...) {
        // Never allow an exception to cross the native callback boundary.
    }
}

class Transport {
public:
    Transport(winrt::handle device, BTH_LE_GATT_CHARACTERISTIC value, std::shared_ptr<Link> link)
        : device_(std::move(device)), value_(value), state_(std::make_shared<Notifications>()),
          callback_id_(next_callback.fetch_add(1)) {
        state_->link = std::move(link);
        {
            std::scoped_lock lock(callback_mutex);
            callbacks.emplace(callback_id_, state_);
        }
        registration_.NumCharacteristics = 1;
        registration_.Characteristics[0] = value_;
        const auto result = BluetoothGATTRegisterEvent(device_.get(), CharacteristicValueChangedEvent,
            &registration_, notification, reinterpret_cast<PVOID>(callback_id_), &registration_handle_, 0);
        if (result != S_OK) {
            unregister();
            check_gatt(result, "BluetoothGATTRegisterEvent");
        }
    }

    ~Transport() {
        listener({});
        unregister();
    }

    bool lost() const { return state_->link && state_->link->lost(); }
    HANDLE lost_event() const { return state_->link->lost_event.get(); }

    void listener(std::function<void(const Packet&)> callback) {
        std::scoped_lock lock(state_->mutex);
        state_->listener = std::move(callback);
    }

    Packet transact(Byte feature, Byte function, std::initializer_list<Byte> parameters) {
        check_link();
        const Byte id = software_id_;
        software_id_ = id == 0x0F ? 1 : static_cast<Byte>(id + 1);
        const auto packet = request(feature, function, id, {parameters.begin(), parameters.size()});
        const auto function_address = packet[1];
        {
            std::scoped_lock lock(state_->mutex);
            state_->feature = feature;
            state_->address = function_address;
            state_->reply.reset();
            ResetEvent(state_->reply_event.get());
        }
        try {
            write(packet);
            const auto reply_event = state_->reply_event.get();
            const bool received = state_->link
                ? wait_any(std::array{lost_event(), reply_event}, reply_ms) == WAIT_OBJECT_0 + 1
                : wait_any(std::array{reply_event}, reply_ms) == WAIT_OBJECT_0;
            check_link();
            if (!received) throw TransportError(std::format(
                "HID++ timed out for feature 0x{:02X}, function {}.", feature, function));
            std::scoped_lock lock(state_->mutex);
            const auto response = state_->reply.value();
            state_->address = 0;
            if (classify(response, feature, function_address) == Reply::device_error)
                throw TransportError(std::format("HID++ error 0x{:02X} for feature 0x{:02X}, function {}.",
                    response[3], feature, function));
            return response;
        } catch (...) {
            std::scoped_lock lock(state_->mutex);
            state_->address = 0;
            throw;
        }
    }

    void ensure_notifications() {
        check_link();
        USHORT count{};
        const auto first = BluetoothGATTGetDescriptors(device_.get(), &value_, 0, nullptr, &count, 0);
        if (first != more_data || !count) throw TransportError("Could not enumerate GATT notification descriptors.");
        std::vector<BTH_LE_GATT_DESCRIPTOR> descriptors(count);
        check_gatt(BluetoothGATTGetDescriptors(device_.get(), &value_, count, descriptors.data(), &count, 0),
            "BluetoothGATTGetDescriptors");
        const auto last = descriptors.begin() + count;
        const auto found = std::find_if(descriptors.begin(), last, [](const auto& descriptor) {
            return descriptor.DescriptorType == ClientCharacteristicConfiguration;
        });
        if (found == last) throw TransportError("GATT notification descriptor was not found.");
        descriptor_ = *found;

        USHORT required{};
        const auto size_result = BluetoothGATTGetDescriptorValue(device_.get(), &descriptor_,
            0, nullptr, &required, BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);
        if (size_result != more_data || required < sizeof(BTH_LE_GATT_DESCRIPTOR_VALUE))
            throw TransportError("Unexpected GATT notification descriptor size.");
        auto buffer = storage(required);
        auto* original = reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(buffer.data());
        check_gatt(BluetoothGATTGetDescriptorValue(device_.get(), &descriptor_, required, original, &required, 0),
            "BluetoothGATTGetDescriptorValue");
        if (original->ClientCharacteristicConfiguration.IsSubscribeToNotification) return;
        auto enabled = *original;
        enabled.ClientCharacteristicConfiguration.IsSubscribeToNotification = TRUE;
        check_link();
        cccd_ = *original;
        check_gatt(BluetoothGATTSetDescriptorValue(device_.get(), &descriptor_, &enabled, 0),
            "BluetoothGATTSetDescriptorValue");
    }

    void restore_notifications() {
        if (!cccd_ || lost()) return;
        check_gatt(BluetoothGATTSetDescriptorValue(device_.get(), &descriptor_, &*cccd_, 0),
            "Restore GATT notification descriptor");
        cccd_.reset();
    }

private:
    void unregister() noexcept {
        {
            std::scoped_lock lock(callback_mutex);
            callbacks.erase(callback_id_);
        }
        if (registration_handle_) BluetoothGATTUnregisterEvent(registration_handle_, 0);
    }

    void check_link() const {
        if (lost()) throw TransportError("The Bluetooth link was lost.");
    }

    void write(const Packet& packet) {
        constexpr auto offset = offsetof(BTH_LE_GATT_CHARACTERISTIC_VALUE, Data);
        alignas(BTH_LE_GATT_CHARACTERISTIC_VALUE) std::array<std::byte, offset + Packet{}.size()> buffer{};
        auto* value = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(buffer.data());
        value->DataSize = static_cast<ULONG>(packet.size());
        std::memcpy(buffer.data() + offset, packet.data(), packet.size());
        check_link();
        check_gatt(BluetoothGATTSetCharacteristicValue(device_.get(), &value_, value, 0, 0),
            "BluetoothGATTSetCharacteristicValue");
    }

    winrt::handle device_;
    BTH_LE_GATT_CHARACTERISTIC value_;
    std::shared_ptr<Notifications> state_;
    std::uintptr_t callback_id_;
    BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION registration_{};
    BLUETOOTH_GATT_EVENT_HANDLE registration_handle_{};
    BTH_LE_GATT_DESCRIPTOR descriptor_{};
    std::optional<BTH_LE_GATT_DESCRIPTOR_VALUE> cccd_;
    Byte software_id_{0x0D};
};

std::unique_ptr<Transport> connect(MonitorState* monitor = nullptr, bool* candidate_online = nullptr) {
    for (const auto& path : candidate_paths()) {
        auto link = monitor ? monitor->acquire(path_address(path)) : nullptr;
        if (monitor && !link) continue;
        if (candidate_online) *candidate_online = true;
        auto device = open_device(path);
        if (!device || (link && link->lost())) continue;
        const auto characteristic = find_characteristic(device.get());
        if (!characteristic || (link && link->lost())) continue;
        return std::make_unique<Transport>(std::move(device), *characteristic, std::move(link));
    }
    return {};
}

Byte read_feature(Transport& transport, const Log& log) {
    const auto challenge = static_cast<Byte>(1 + (GetTickCount64() & 0x7F));
    const auto ping = transport.transact(0, 1, {0, 0, challenge});
    if (ping[2] < 2 || ping[4] != challenge) throw TransportError("Invalid HID++ root ping.");
    log(std::format("HID++ {}.{}", ping[2], ping[3]));
    const auto response = transport.transact(0, 0, {0x1B, 0x04});
    if (!response[2]) throw TransportError("HID++ feature 0x1B04 was not found.");
    log(std::format("0x1B04: index 0x{:02X}, version {}", response[2], response[4]));
    return response[2];
}

Reporting read_reporting(Transport& transport, Byte feature, Cid cid) {
    const auto response = transport.transact(feature, 2, {static_cast<Byte>(cid >> 8), static_cast<Byte>(cid)});
    const auto state = reporting(response);
    if (state.cid != cid) throw TransportError(std::format("Invalid reporting response for CID 0x{:04X}.", cid));
    return state;
}

Snapshot read_snapshot(Transport& transport, Byte feature, const Log& log) {
    Snapshot snapshot{};
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto state = read_reporting(transport, feature, targets[i]);
        snapshot[i] = state;
        log(std::format("CID 0x{:04X}: flags=0x{:02X}, remap=0x{:04X}", state.cid, state.flags, state.remap));
    }
    return snapshot;
}

void set_divert(Transport& transport, Byte feature, Reporting baseline, bool enabled) {
    transport.transact(feature, 3, {static_cast<Byte>(baseline.cid >> 8), static_cast<Byte>(baseline.cid),
        static_cast<Byte>(enabled ? 3 : 2), 0, 0});
    // The M720 acknowledges with zeroed parameters. Read the result back.
    const auto state = read_reporting(transport, feature, baseline.cid);
    if (state.flags != baseline.with_divert(enabled) || state.remap != baseline.remap)
        throw TransportError(std::format("Unexpected reporting state for CID 0x{:04X}.", baseline.cid));
}

INPUT key(WORD vk, DWORD flags = 0) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = flags;
    return input;
}

void send(Cid cid, const Log& log) {
    std::array<INPUT, 4> inputs;
    UINT count;
    if (cid == thumb) {
        count = 2;
        inputs[0] = key(VK_SNAPSHOT, KEYEVENTF_EXTENDEDKEY);
        inputs[1] = key(VK_SNAPSHOT, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP);
    } else {
        count = 4;
        const WORD letter = cid == forward ? 'C' : 'V';
        inputs = {key(VK_CONTROL), key(letter), key(letter, KEYEVENTF_KEYUP), key(VK_CONTROL, KEYEVENTF_KEYUP)};
    }
    const auto sent = SendInput(count, inputs.data(), sizeof(INPUT));
    if (sent != count) log(std::format("SendInput delivered {} of {} events (error {}).", sent, count, GetLastError()));
}

void mapped_session(Transport& transport, HANDLE stop, HANDLE active, const Log& log) {
    std::optional<ReportingPlan> plan;
    Byte feature{};
    std::exception_ptr failure;
    try {
        transport.ensure_notifications();
        feature = read_feature(transport, log);
        plan.emplace(read_snapshot(transport, feature, log));
        while (plan->has_unarmed() && !signaled(stop) && !transport.lost())
            set_divert(transport, feature, plan->next_to_arm(), true);

        if (!signaled(stop) && !transport.lost()) {
            transport.listener([buttons = Buttons(feature), &log](const Packet& packet) mutable {
                for (const auto cid : buttons.update(packet)) if (cid) send(cid, log);
            });
            SetEvent(active);
            log("Active: Forward=Ctrl+C, Back=Ctrl+V, Thumb=Print Screen.");
            wait_any(std::array{stop, transport.lost_event()});
        }
    } catch (...) { failure = std::current_exception(); }

    ResetEvent(active);
    transport.listener({});
    bool restore_failed = false;
    while (plan && plan->has_to_restore() && !transport.lost()) {
        const auto state = plan->next_to_restore();
        try { set_divert(transport, feature, state, state.diverted()); }
        catch (const TransportError& ex) {
            if (transport.lost()) break;
            restore_failed = true;
            log(std::format("Could not restore CID 0x{:04X}: {}", state.cid, ex.what()));
        }
    }
    try { transport.restore_notifications(); }
    catch (const TransportError& ex) {
        if (!transport.lost()) {
            restore_failed = true;
            log(ex.what());
        }
    }
    if (restore_failed) throw RestoreError("Bluetooth reporting restoration was incomplete.");
    if (failure && !transport.lost()) std::rethrow_exception(failure);
}

} // namespace

void run_mapper(HANDLE stop, HANDLE active, HANDLE waiting, const Log& log) {
    winrt::init_apartment();
    struct ApartmentGuard { ~ApartmentGuard() { winrt::uninit_apartment(); } } apartment;
    log("Waiting for the M720 over Bluetooth.");
    while (!signaled(stop)) {
        Monitor monitor;
        auto& state = monitor.state();
        std::string last_error;
        while (!signaled(stop) && !state.is_stopped()) {
            bool candidate_online = false;
            try {
                auto transport = connect(&state, &candidate_online);
                if (candidate_online) ResetEvent(waiting);
                if (transport) {
                    mapped_session(*transport, stop, active, log);
                    if (transport->lost()) {
                        log("M720 disconnected. Waiting for it to return.");
                        last_error.clear();
                        continue;
                    }
                }
            } catch (const TransportError& ex) {
                if (signaled(stop)) throw;
                if (last_error != ex.what()) {
                    last_error = ex.what();
                    log(std::format("M720: {} Retrying.", last_error));
                }
            }
            if (signaled(stop) || state.is_stopped()) break;
            const bool online = candidate_online || state.model_online();
            if (!online && state.initial_enumeration_done()) SetEvent(waiting);
            else ResetEvent(waiting);
            wait_any(std::array{stop, state.changed.get()}, online ? retry_ms : INFINITE);
        }
        ResetEvent(waiting);
        if (!signaled(stop)) {
            log("Windows Bluetooth monitor stopped. Restarting it.");
            wait_any(std::array{stop}, retry_ms);
        }
    }
    log("M720 Mapper stopped.");
}

void diagnose(const Log& log) {
    log("M720 Bluetooth GATT diagnostic (read-only).");
    unsigned found = 0;
    for (const auto& path : candidate_paths()) {
        auto device = open_device(path);
        if (!device) continue;
        auto characteristic = find_characteristic(device.get());
        if (!characteristic) continue;
        ++found;
        log(std::format("Logitech service and characteristic found (value handle 0x{:04X}).", characteristic->CharacteristicValueHandle));
        USHORT required{};
        auto buffer = storage(516);
        auto* value = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(buffer.data());
        const auto result = BluetoothGATTGetCharacteristicValue(device.get(), &*characteristic, 516,
            value, &required, BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);
        if (result == S_OK && value->DataSize <= 512) {
            const auto* bytes = reinterpret_cast<const Byte*>(buffer.data()) + offsetof(BTH_LE_GATT_CHARACTERISTIC_VALUE, Data);
            std::string output("  Value:");
            for (ULONG i = 0; i < value->DataSize; ++i) output += std::format(" {:02X}", bytes[i]);
            log(output);
        } else log(std::format("  Characteristic read result: 0x{:08X}", static_cast<unsigned>(result)));
    }
    if (!found) throw TransportError("M720 Bluetooth PID B015 GATT service was not found.");
}

void probe(const Log& log) {
    auto transport = connect();
    if (!transport) throw TransportError("M720 Bluetooth PID B015 GATT service was not found.");
    const auto feature = read_feature(*transport, log);
    read_snapshot(*transport, feature, log);
}

void listen(std::chrono::seconds duration, const Log& log) {
    auto transport = connect();
    if (!transport) throw TransportError("M720 Bluetooth PID B015 GATT service was not found.");
    const auto feature = read_feature(*transport, log);
    read_snapshot(*transport, feature, log);
    transport->listener([buttons = Buttons(feature), &log](const Packet& packet) mutable {
        for (const auto cid : buttons.update(packet)) if (cid) log(std::format("Pressed CID 0x{:04X}", cid));
    });
    log(std::format("Listening for {} seconds. Only already-diverted buttons can appear.", duration.count()));
    Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()));
    transport->listener({});
}

} // namespace m720
